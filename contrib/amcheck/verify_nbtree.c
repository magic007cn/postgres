/*-------------------------------------------------------------------------
 *
 * verify_nbtree.c
 *		Verifies the integrity of nbtree indexes based on invariants.
 *
 * For B-Tree indexes, verification includes checking that each page in the
 * target index has items in logical order as reported by an insertion scankey
 * (the insertion scankey sort-wise NULL semantics are needed for
 * verification).
 *
 * When index-to-heap verification is requested, a Bloom filter is used to
 * fingerprint all tuples in the target index, as the index is traversed to
 * verify its structure.  A heap scan later uses Bloom filter probes to verify
 * that every visible heap tuple has a matching index tuple.
 *
 *
 * Copyright (c) 2017-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_nbtree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "commands/tablecmds.h"
#include "lib/bloomfilter.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


PG_MODULE_MAGIC;

/*
 * A B-Tree cannot possibly have this many levels, since there must be one
 * block per level, which is bound by the range of BlockNumber:
 */
#define InvalidBtreeLevel	((uint32) InvalidBlockNumber)
#define BTreeTupleGetNKeyAtts(itup, rel)   \
	Min(IndexRelationGetNumberOfKeyAttributes(rel), BTreeTupleGetNAtts(itup, rel))

/*
 * State associated with verifying a B-Tree index
 *
 * target is the point of reference for a verification operation.
 *
 * Other B-Tree pages may be allocated, but those are always auxiliary (e.g.,
 * they are current target's child pages).  Conceptually, problems are only
 * ever found in the current target page (or for a particular heap tuple during
 * heapallindexed verification).  Each page found by verification's left/right,
 * top/bottom scan becomes the target exactly once.
 */
typedef struct BtreeCheckState
{
	/*
	 * Unchanging state, established at start of verification:
	 */

	/* B-Tree Index Relation and associated heap relation */
	Relation	rel;
	Relation	heaprel;
	/* rel is heapkeyspace index? */
	bool		heapkeyspace;
	/* ShareLock held on heap/index, rather than AccessShareLock? */
	bool		readonly;
	/* Also verifying heap has no unindexed tuples? */
	bool		heapallindexed;
	/* Also making sure non-pivot tuples can be found by new search? */
	bool		rootdescend;
	/* Per-page context */
	MemoryContext targetcontext;
	/* Buffer access strategy */
	BufferAccessStrategy checkstrategy;

	/*
	 * Mutable state, for verification of particular page:
	 */

	/* Current target page */
	Page		target;
	/* Target block number */
	BlockNumber targetblock;
	/* Target page's LSN */
	XLogRecPtr	targetlsn;

	/*
	 * Mutable state, for optional heapallindexed verification:
	 */

	/* Bloom filter fingerprints B-Tree index */
	bloom_filter *filter;
	/* Bloom filter fingerprints downlink blocks within tree */
	bloom_filter *downlinkfilter;
	/* Right half of incomplete split marker */
	bool		rightsplit;
	/* Debug counter */
	int64		heaptuplespresent;
} BtreeCheckState;

/*
 * Starting point for verifying an entire B-Tree index level
 */
typedef struct BtreeLevel
{
	/* Level number (0 is leaf page level). */
	uint32		level;

	/* Left most block on level.  Scan of level begins here. */
	BlockNumber leftmost;

	/* Is this level reported as "true" root level by meta page? */
	bool		istruerootlevel;
} BtreeLevel;

PG_FUNCTION_INFO_V1(bt_index_check);
PG_FUNCTION_INFO_V1(bt_index_parent_check);

static void bt_index_check_internal(Oid indrelid, bool parentcheck,
									bool heapallindexed, bool rootdescend);
static inline void btree_index_checkable(Relation rel);
static inline bool btree_index_mainfork_expected(Relation rel);
static void bt_check_every_level(Relation rel, Relation heaprel,
								 bool heapkeyspace, bool readonly, bool heapallindexed,
								 bool rootdescend);
static BtreeLevel bt_check_level_from_leftmost(BtreeCheckState *state,
											   BtreeLevel level);
static bool bt_leftmost_ignoring_half_dead(BtreeCheckState *state,
										   BlockNumber start,
										   BTPageOpaque start_opaque);
static void bt_target_page_check(BtreeCheckState *state);
static BTScanInsert bt_right_page_check_scankey(BtreeCheckState *state);
static void bt_downlink_check(BtreeCheckState *state, BTScanInsert targetkey,
							  BlockNumber childblock);
static void bt_downlink_missing_check(BtreeCheckState *state);
static void bt_tuple_present_callback(Relation index, HeapTuple htup,
									  Datum *values, bool *isnull,
									  bool tupleIsAlive, void *checkstate);
static IndexTuple bt_normalize_tuple(BtreeCheckState *state,
									 IndexTuple itup);
static bool bt_rootdescend(BtreeCheckState *state, IndexTuple itup);
static inline bool offset_is_negative_infinity(BTPageOpaque opaque,
											   OffsetNumber offset);
static inline bool invariant_l_offset(BtreeCheckState *state, BTScanInsert key,
									  OffsetNumber upperbound);
static inline bool invariant_leq_offset(BtreeCheckState *state,
										BTScanInsert key,
										OffsetNumber upperbound);
static inline bool invariant_g_offset(BtreeCheckState *state, BTScanInsert key,
									  OffsetNumber lowerbound);
static inline bool invariant_l_nontarget_offset(BtreeCheckState *state,
												BTScanInsert key,
												BlockNumber nontargetblock,
												Page nontarget,
												OffsetNumber upperbound);
static Page palloc_btree_page(BtreeCheckState *state, BlockNumber blocknum);
static inline BTScanInsert bt_mkscankey_pivotsearch(Relation rel,
													IndexTuple itup);
static ItemId PageGetItemIdCareful(BtreeCheckState *state, BlockNumber block,
								   Page page, OffsetNumber offset);
static inline ItemPointer BTreeTupleGetHeapTIDCareful(BtreeCheckState *state,
													  IndexTuple itup, bool nonpivot);

/*
 * bt_index_check(index regclass, heapallindexed boolean)
 *
 * Verify integrity of B-Tree index.
 *
 * Acquires AccessShareLock on heap & index relations.  Does not consider
 * invariants that exist between parent/child pages.  Optionally verifies
 * that heap does not contain any unindexed or incorrectly indexed tuples.
 */
Datum
bt_index_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);
	bool		heapallindexed = false;

	if (PG_NARGS() == 2)
		heapallindexed = PG_GETARG_BOOL(1);

	bt_index_check_internal(indrelid, false, heapallindexed, false);

	PG_RETURN_VOID();
}

/*
 * bt_index_parent_check(index regclass, heapallindexed boolean)
 *
 * Verify integrity of B-Tree index.
 *
 * Acquires ShareLock on heap & index relations.  Verifies that downlinks in
 * parent pages are valid lower bounds on child pages.  Optionally verifies
 * that heap does not contain any unindexed or incorrectly indexed tuples.
 */
Datum
bt_index_parent_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);
	bool		heapallindexed = false;
	bool		rootdescend = false;

	if (PG_NARGS() >= 2)
		heapallindexed = PG_GETARG_BOOL(1);
	if (PG_NARGS() == 3)
		rootdescend = PG_GETARG_BOOL(2);

	bt_index_check_internal(indrelid, true, heapallindexed, rootdescend);

	PG_RETURN_VOID();
}

/*
 * Helper for bt_index_[parent_]check, coordinating the bulk of the work.
 */
static void
bt_index_check_internal(Oid indrelid, bool parentcheck, bool heapallindexed,
						bool rootdescend)
{
	Oid			heapid;
	Relation	indrel;
	Relation	heaprel;
	LOCKMODE	lockmode;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	if (parentcheck)
		lockmode = ShareLock;
	else
		lockmode = AccessShareLock;

	/*
	 * We must lock table before index to avoid deadlocks.  However, if the
	 * passed indrelid isn't an index then IndexGetRelation() will fail.
	 * Rather than emitting a not-very-helpful error message, postpone
	 * complaining, expecting that the is-it-an-index test below will fail.
	 *
	 * In hot standby mode this will raise an error when parentcheck is true.
	 */
	heapid = IndexGetRelation(indrelid, true);
	if (OidIsValid(heapid))
	{
		heaprel = table_open(heapid, lockmode);

		/*
		 * Switch to the table owner's userid, so that any index functions are
		 * run as that user.  Also lock down security-restricted operations
		 * and arrange to make GUC variable changes local to this command.
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(heaprel->rd_rel->relowner,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();
	}
	else
	{
		heaprel = NULL;
		/* Set these just to suppress "uninitialized variable" warnings */
		save_userid = InvalidOid;
		save_sec_context = -1;
		save_nestlevel = -1;
	}

	/*
	 * Open the target index relations separately (like relation_openrv(), but
	 * with heap relation locked first to prevent deadlocking).  In hot
	 * standby mode this will raise an error when parentcheck is true.
	 *
	 * There is no need for the usual indcheckxmin usability horizon test
	 * here, even in the heapallindexed case, because index undergoing
	 * verification only needs to have entries for a new transaction snapshot.
	 * (If this is a parentcheck verification, there is no question about
	 * committed or recently dead heap tuples lacking index entries due to
	 * concurrent activity.)
	 */
	indrel = index_open(indrelid, lockmode);

	/*
	 * Since we did the IndexGetRelation call above without any lock, it's
	 * barely possible that a race against an index drop/recreation could have
	 * netted us the wrong table.
	 */
	if (heaprel == NULL || heapid != IndexGetRelation(indrelid, false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index %s",
						RelationGetRelationName(indrel))));

	/* Relation suitable for checking as B-Tree? */
	btree_index_checkable(indrel);

	if (btree_index_mainfork_expected(indrel))
	{
		bool	heapkeyspace;

		if (!smgrexists(RelationGetSmgr(indrel), MAIN_FORKNUM))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" lacks a main relation fork",
							RelationGetRelationName(indrel))));

		/* Check index, possibly against table it is an index on */
		heapkeyspace = _bt_heapkeyspace(indrel);
		bt_check_every_level(indrel, heaprel, heapkeyspace, parentcheck,
							 heapallindexed, rootdescend);
	}

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/*
	 * Release locks early. That's ok here because nothing in the called
	 * routines will trigger shared cache invalidations to be sent, so we can
	 * relax the usual pattern of only releasing locks after commit.
	 */
	index_close(indrel, lockmode);
	if (heaprel)
		table_close(heaprel, lockmode);
}

/*
 * Basic checks about the suitability of a relation for checking as a B-Tree
 * index.
 *
 * NB: Intentionally not checking permissions, the function is normally not
 * callable by non-superusers. If granted, it's useful to be able to check a
 * whole cluster.
 */
static inline void
btree_index_checkable(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_INDEX ||
		rel->rd_rel->relam != BTREE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only B-Tree indexes are supported as targets for verification"),
				 errdetail("Relation \"%s\" is not a B-Tree index.",
						   RelationGetRelationName(rel))));

	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions"),
				 errdetail("Index \"%s\" is associated with temporary relation.",
						   RelationGetRelationName(rel))));

	if (!rel->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot check index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Index is not valid.")));
}

/*
 * Check if B-Tree index relation should have a file for its main relation
 * fork.  Verification uses this to skip unlogged indexes when in hot standby
 * mode, where there is simply nothing to verify.
 *
 * NB: Caller should call btree_index_checkable() before calling here.
 */
static inline bool
btree_index_mainfork_expected(Relation rel)
{
	if (rel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED ||
		!RecoveryInProgress())
		return true;

	ereport(NOTICE,
			(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
			 errmsg("cannot verify unlogged index \"%s\" during recovery, skipping",
					RelationGetRelationName(rel))));

	return false;
}

/*
 * Main entry point for B-Tree SQL-callable functions. Walks the B-Tree in
 * logical order, verifying invariants as it goes.  Optionally, verification
 * checks if the heap relation contains any tuples that are not represented in
 * the index but should be.
 *
 * It is the caller's responsibility to acquire appropriate heavyweight lock on
 * the index relation, and advise us if extra checks are safe when a ShareLock
 * is held.  (A lock of the same type must also have been acquired on the heap
 * relation.)
 *
 * A ShareLock is generally assumed to prevent any kind of physical
 * modification to the index structure, including modifications that VACUUM may
 * make.  This does not include setting of the LP_DEAD bit by concurrent index
 * scans, although that is just metadata that is not able to directly affect
 * any check performed here.  Any concurrent process that might act on the
 * LP_DEAD bit being set (recycle space) requires a heavyweight lock that
 * cannot be held while we hold a ShareLock.  (Besides, even if that could
 * happen, the ad-hoc recycling when a page might otherwise split is performed
 * per-page, and requires an exclusive buffer lock, which wouldn't cause us
 * trouble.  _bt_delitems_vacuum() may only delete leaf items, and so the extra
 * parent/child check cannot be affected.)
 */
static void
bt_check_every_level(Relation rel, Relation heaprel, bool heapkeyspace,
					 bool readonly, bool heapallindexed, bool rootdescend)
{
	BtreeCheckState *state;
	Page		metapage;
	BTMetaPageData *metad;
	uint32		previouslevel;
	BtreeLevel	current;
	Snapshot	snapshot = SnapshotAny;

	/*
	 * RecentGlobalXmin assertion matches index_getnext_tid().  See note on
	 * RecentGlobalXmin/B-Tree page deletion.
	 */
	Assert(TransactionIdIsValid(RecentGlobalXmin));

	/*
	 * Initialize state for entire verification operation
	 */
	state = palloc0(sizeof(BtreeCheckState));
	state->rel = rel;
	state->heaprel = heaprel;
	state->heapkeyspace = heapkeyspace;
	state->readonly = readonly;
	state->heapallindexed = heapallindexed;
	state->rootdescend = rootdescend;

	if (state->heapallindexed)
	{
		int64		total_pages;
		int64		total_elems;
		uint64		seed;

		/*
		 * Size Bloom filter based on estimated number of tuples in index,
		 * while conservatively assuming that each block must contain at least
		 * MaxIndexTuplesPerPage / 5 non-pivot tuples.  (Non-leaf pages cannot
		 * contain non-pivot tuples.  That's okay because they generally make
		 * up no more than about 1% of all pages in the index.)
		 */
		total_pages = RelationGetNumberOfBlocks(rel);
		total_elems = Max(total_pages * (MaxIndexTuplesPerPage / 5),
						  (int64) state->rel->rd_rel->reltuples);
		/* Random seed relies on backend srandom() call to avoid repetition */
		seed = random();
		/* Create Bloom filter to fingerprint index */
		state->filter = bloom_create(total_elems, maintenance_work_mem, seed);
		state->heaptuplespresent = 0;

		/*
		 * Register our own snapshot in !readonly case, rather than asking
		 * table_index_build_scan() to do this for us later.  This needs to
		 * happen before index fingerprinting begins, so we can later be
		 * certain that index fingerprinting should have reached all tuples
		 * returned by table_index_build_scan().
		 *
		 * In readonly case, we also check for problems with missing
		 * downlinks. A second Bloom filter is used for this.
		 */
		if (!state->readonly)
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());

			/*
			 * GetTransactionSnapshot() always acquires a new MVCC snapshot in
			 * READ COMMITTED mode.  A new snapshot is guaranteed to have all
			 * the entries it requires in the index.
			 *
			 * We must defend against the possibility that an old xact
			 * snapshot was returned at higher isolation levels when that
			 * snapshot is not safe for index scans of the target index.  This
			 * is possible when the snapshot sees tuples that are before the
			 * index's indcheckxmin horizon.  Throwing an error here should be
			 * very rare.  It doesn't seem worth using a secondary snapshot to
			 * avoid this.
			 */
			if (IsolationUsesXactSnapshot() && rel->rd_index->indcheckxmin &&
				!TransactionIdPrecedes(HeapTupleHeaderGetXmin(rel->rd_indextuple->t_data),
									   snapshot->xmin))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("index \"%s\" cannot be verified using transaction snapshot",
								RelationGetRelationName(rel))));
		}
		else
		{
			/*
			 * Extra readonly downlink check.
			 *
			 * In readonly case, we know that there cannot be a concurrent
			 * page split or a concurrent page deletion, which gives us the
			 * opportunity to verify that every non-ignorable page had a
			 * downlink one level up.  We must be tolerant of interrupted page
			 * splits and page deletions, though.  This is taken care of in
			 * bt_downlink_missing_check().
			 */
			state->downlinkfilter = bloom_create(total_pages, work_mem, seed);
		}
	}

	Assert(!state->rootdescend || state->readonly);
	if (state->rootdescend && !state->heapkeyspace)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot verify that tuples from index \"%s\" can each be found by an independent index search",
						RelationGetRelationName(rel)),
				 errhint("Only B-Tree version 4 indexes support rootdescend verification.")));

	/* Create context for page */
	state->targetcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "amcheck context",
												 ALLOCSET_DEFAULT_SIZES);
	state->checkstrategy = GetAccessStrategy(BAS_BULKREAD);

	/* Get true root block from meta-page */
	metapage = palloc_btree_page(state, BTREE_METAPAGE);
	metad = BTPageGetMeta(metapage);

	/*
	 * Certain deletion patterns can result in "skinny" B-Tree indexes, where
	 * the fast root and true root differ.
	 *
	 * Start from the true root, not the fast root, unlike conventional index
	 * scans.  This approach is more thorough, and removes the risk of
	 * following a stale fast root from the meta page.
	 */
	if (metad->btm_fastroot != metad->btm_root)
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg("harmless fast root mismatch in index %s",
						RelationGetRelationName(rel)),
				 errdetail_internal("Fast root block %u (level %u) differs from true root block %u (level %u).",
									metad->btm_fastroot, metad->btm_fastlevel,
									metad->btm_root, metad->btm_level)));

	/*
	 * Starting at the root, verify every level.  Move left to right, top to
	 * bottom.  Note that there may be no pages other than the meta page (meta
	 * page can indicate that root is P_NONE when the index is totally empty).
	 */
	previouslevel = InvalidBtreeLevel;
	current.level = metad->btm_level;
	current.leftmost = metad->btm_root;
	current.istruerootlevel = true;
	while (current.leftmost != P_NONE)
	{
		/*
		 * Leftmost page on level cannot be right half of incomplete split.
		 * This can go stale immediately in !readonly case.
		 */
		state->rightsplit = false;

		/*
		 * Verify this level, and get left most page for next level down, if
		 * not at leaf level
		 */
		current = bt_check_level_from_leftmost(state, current);

		if (current.leftmost == InvalidBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has no valid pages on level below %u or first level",
							RelationGetRelationName(rel), previouslevel)));

		previouslevel = current.level;
	}

	/*
	 * * Check whether heap contains unindexed/malformed tuples *
	 */
	if (state->heapallindexed)
	{
		IndexInfo  *indexinfo = BuildIndexInfo(state->rel);
		TableScanDesc scan;

		/* Report on extra downlink checks performed in readonly case */
		if (state->readonly)
		{
			ereport(DEBUG1,
					(errmsg_internal("finished verifying presence of downlink blocks within index \"%s\" with bitset %.2f%% set",
									 RelationGetRelationName(rel),
									 100.0 * bloom_prop_bits_set(state->downlinkfilter))));
			bloom_free(state->downlinkfilter);
		}

		/*
		 * Create our own scan for table_index_build_scan(), rather than
		 * getting it to do so for us.  This is required so that we can
		 * actually use the MVCC snapshot registered earlier in !readonly
		 * case.
		 *
		 * Note that table_index_build_scan() calls heap_endscan() for us.
		 */
		scan = table_beginscan_strat(state->heaprel,	/* relation */
									 snapshot,	/* snapshot */
									 0, /* number of keys */
									 NULL,	/* scan key */
									 true,	/* buffer access strategy OK */
									 true); /* syncscan OK? */

		/*
		 * Scan will behave as the first scan of a CREATE INDEX CONCURRENTLY
		 * behaves in !readonly case.
		 *
		 * It's okay that we don't actually use the same lock strength for the
		 * heap relation as any other ii_Concurrent caller would in !readonly
		 * case.  We have no reason to care about a concurrent VACUUM
		 * operation, since there isn't going to be a second scan of the heap
		 * that needs to be sure that there was no concurrent recycling of
		 * TIDs.
		 */
		indexinfo->ii_Concurrent = !state->readonly;

		/*
		 * Don't wait for uncommitted tuple xact commit/abort when index is a
		 * unique index on a catalog (or an index used by an exclusion
		 * constraint).  This could otherwise happen in the readonly case.
		 */
		indexinfo->ii_Unique = false;
		indexinfo->ii_ExclusionOps = NULL;
		indexinfo->ii_ExclusionProcs = NULL;
		indexinfo->ii_ExclusionStrats = NULL;

		elog(DEBUG1, "verifying that tuples from index \"%s\" are present in \"%s\"",
			 RelationGetRelationName(state->rel),
			 RelationGetRelationName(state->heaprel));

		table_index_build_scan(state->heaprel, state->rel, indexinfo, true, false,
							   bt_tuple_present_callback, (void *) state, scan);

		ereport(DEBUG1,
				(errmsg_internal("finished verifying presence of " INT64_FORMAT " tuples from table \"%s\" with bitset %.2f%% set",
								 state->heaptuplespresent, RelationGetRelationName(heaprel),
								 100.0 * bloom_prop_bits_set(state->filter))));

		if (snapshot != SnapshotAny)
			UnregisterSnapshot(snapshot);

		bloom_free(state->filter);
	}

	/* Be tidy: */
	MemoryContextDelete(state->targetcontext);
}

/*
 * Given a left-most block at some level, move right, verifying each page
 * individually (with more verification across pages for "readonly"
 * callers).  Caller should pass the true root page as the leftmost initially,
 * working their way down by passing what is returned for the last call here
 * until level 0 (leaf page level) was reached.
 *
 * Returns state for next call, if any.  This includes left-most block number
 * one level lower that should be passed on next level/call, which is set to
 * P_NONE on last call here (when leaf level is verified).  Level numbers
 * follow the nbtree convention: higher levels have higher numbers, because new
 * levels are added only due to a root page split.  Note that prior to the
 * first root page split, the root is also a leaf page, so there is always a
 * level 0 (leaf level), and it's always the last level processed.
 *
 * Note on memory management:  State's per-page context is reset here, between
 * each call to bt_target_page_check().
 */
static BtreeLevel
bt_check_level_from_leftmost(BtreeCheckState *state, BtreeLevel level)
{
	/* State to establish early, concerning entire level */
	BTPageOpaque opaque;
	MemoryContext oldcontext;
	BtreeLevel	nextleveldown;

	/* Variables for iterating across level using right links */
	BlockNumber leftcurrent = P_NONE;
	BlockNumber current = level.leftmost;

	/* Initialize return state */
	nextleveldown.leftmost = InvalidBlockNumber;
	nextleveldown.level = InvalidBtreeLevel;
	nextleveldown.istruerootlevel = false;

	/* Use page-level context for duration of this call */
	oldcontext = MemoryContextSwitchTo(state->targetcontext);

	elog(DEBUG2, "verifying level %u%s", level.level,
		 level.istruerootlevel ?
		 " (true root level)" : level.level == 0 ? " (leaf level)" : "");

	do
	{
		/* Don't rely on CHECK_FOR_INTERRUPTS() calls at lower level */
		CHECK_FOR_INTERRUPTS();

		/* Initialize state for this iteration */
		state->targetblock = current;
		state->target = palloc_btree_page(state, state->targetblock);
		state->targetlsn = PageGetLSN(state->target);

		opaque = (BTPageOpaque) PageGetSpecialPointer(state->target);

		if (P_IGNORE(opaque))
		{
			/*
			 * Since there cannot be a concurrent VACUUM operation in readonly
			 * mode, and since a page has no links within other pages
			 * (siblings and parent) once it is marked fully deleted, it
			 * should be impossible to land on a fully deleted page in
			 * readonly mode. See bt_downlink_check() for further details.
			 *
			 * The bt_downlink_check() P_ISDELETED() check is repeated here so
			 * that pages that are only reachable through sibling links get
			 * checked.
			 */
			if (state->readonly && P_ISDELETED(opaque))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("downlink or sibling link points to deleted block in index \"%s\"",
								RelationGetRelationName(state->rel)),
						 errdetail_internal("Block=%u left block=%u left link from block=%u.",
											current, leftcurrent, opaque->btpo_prev)));

			if (P_RIGHTMOST(opaque))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("block %u fell off the end of index \"%s\"",
								current, RelationGetRelationName(state->rel))));
			else
				ereport(DEBUG1,
						(errcode(ERRCODE_NO_DATA),
						 errmsg("block %u of index \"%s\" ignored",
								current, RelationGetRelationName(state->rel))));
			goto nextpage;
		}
		else if (nextleveldown.leftmost == InvalidBlockNumber)
		{
			/*
			 * A concurrent page split could make the caller supplied leftmost
			 * block no longer contain the leftmost page, or no longer be the
			 * true root, but where that isn't possible due to heavyweight
			 * locking, check that the first valid page meets caller's
			 * expectations.
			 */
			if (state->readonly)
			{
				if (!bt_leftmost_ignoring_half_dead(state, current, opaque))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("block %u is not leftmost in index \"%s\"",
									current, RelationGetRelationName(state->rel))));

				if (level.istruerootlevel && !P_ISROOT(opaque))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("block %u is not true root in index \"%s\"",
									current, RelationGetRelationName(state->rel))));
			}

			/*
			 * Before beginning any non-trivial examination of level, prepare
			 * state for next bt_check_level_from_leftmost() invocation for
			 * the next level for the next level down (if any).
			 *
			 * There should be at least one non-ignorable page per level,
			 * unless this is the leaf level, which is assumed by caller to be
			 * final level.
			 */
			if (!P_ISLEAF(opaque))
			{
				IndexTuple	itup;
				ItemId		itemid;

				/* Internal page -- downlink gets leftmost on next level */
				itemid = PageGetItemIdCareful(state, state->targetblock,
											  state->target,
											  P_FIRSTDATAKEY(opaque));
				itup = (IndexTuple) PageGetItem(state->target, itemid);
				nextleveldown.leftmost = BTreeInnerTupleGetDownLink(itup);
				nextleveldown.level = opaque->btpo.level - 1;
			}
			else
			{
				/*
				 * Leaf page -- final level caller must process.
				 *
				 * Note that this could also be the root page, if there has
				 * been no root page split yet.
				 */
				nextleveldown.leftmost = P_NONE;
				nextleveldown.level = InvalidBtreeLevel;
			}

			/*
			 * Finished setting up state for this call/level.  Control will
			 * never end up back here in any future loop iteration for this
			 * level.
			 */
		}

		/*
		 * Sibling links should be in mutual agreement.  There arises
		 * leftcurrent == P_NONE && btpo_prev != P_NONE when the left sibling
		 * of the parent's low-key downlink is half-dead.  (A half-dead page
		 * has no downlink from its parent.)  Under heavyweight locking, the
		 * last bt_leftmost_ignoring_half_dead() validated this btpo_prev.
		 */
		if (state->readonly &&
			opaque->btpo_prev != leftcurrent && leftcurrent != P_NONE)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("left link/right link pair in index \"%s\" not in agreement",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Block=%u left block=%u left link from block=%u.",
										current, leftcurrent, opaque->btpo_prev)));

		/* Check level, which must be valid for non-ignorable page */
		if (level.level != opaque->btpo.level)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("leftmost down link for level points to block in index \"%s\" whose level is not one level down",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Block pointed to=%u expected level=%u level in pointed to block=%u.",
										current, level.level, opaque->btpo.level)));

		/* Verify invariants for page */
		bt_target_page_check(state);

nextpage:

		/* Try to detect circular links */
		if (current == leftcurrent || current == opaque->btpo_prev)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("circular link chain found in block %u of index \"%s\"",
							current, RelationGetRelationName(state->rel))));

		/*
		 * Record if page that is about to become target is the right half of
		 * an incomplete page split.  This can go stale immediately in
		 * !readonly case.
		 */
		state->rightsplit = P_INCOMPLETE_SPLIT(opaque);

		leftcurrent = current;
		current = opaque->btpo_next;

		/* Free page and associated memory for this iteration */
		MemoryContextReset(state->targetcontext);
	}
	while (current != P_NONE);

	/* Don't change context for caller */
	MemoryContextSwitchTo(oldcontext);

	return nextleveldown;
}

/*
 * Like P_LEFTMOST(start_opaque), but accept an arbitrarily-long chain of
 * half-dead, sibling-linked pages to the left.  If a half-dead page appears
 * under state->readonly, the database exited recovery between the first-stage
 * and second-stage WAL records of a deletion.
 */
static bool
bt_leftmost_ignoring_half_dead(BtreeCheckState *state,
							   BlockNumber start,
							   BTPageOpaque start_opaque)
{
	BlockNumber reached = start_opaque->btpo_prev,
				reached_from = start;
	bool		all_half_dead = true;

	/*
	 * To handle the !readonly case, we'd need to accept BTP_DELETED pages and
	 * potentially observe nbtree/README "Page deletion and backwards scans".
	 */
	Assert(state->readonly);

	while (reached != P_NONE && all_half_dead)
	{
		Page		page = palloc_btree_page(state, reached);
		BTPageOpaque reached_opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		CHECK_FOR_INTERRUPTS();

		/*
		 * Try to detect btpo_prev circular links.  _bt_unlink_halfdead_page()
		 * writes that side-links will continue to point to the siblings.
		 * Check btpo_next for that property.
		 */
		all_half_dead = P_ISHALFDEAD(reached_opaque) &&
			reached != start &&
			reached != reached_from &&
			reached_opaque->btpo_next == reached_from;
		if (all_half_dead)
		{
			XLogRecPtr	pagelsn = PageGetLSN(page);

			/* pagelsn should point to an XLOG_BTREE_MARK_PAGE_HALFDEAD */
			ereport(DEBUG1,
					(errcode(ERRCODE_NO_DATA),
					 errmsg_internal("harmless interrupted page deletion detected in index \"%s\"",
									 RelationGetRelationName(state->rel)),
					 errdetail_internal("Block=%u right block=%u page lsn=%X/%X.",
										reached, reached_from,
										(uint32) (pagelsn >> 32),
										(uint32) pagelsn)));

			reached_from = reached;
			reached = reached_opaque->btpo_prev;
		}

		pfree(page);
	}

	return all_half_dead;
}

/*
 * Function performs the following checks on target page, or pages ancillary to
 * target page:
 *
 * - That every "real" data item is less than or equal to the high key, which
 *	 is an upper bound on the items on the page.  Data items should be
 *	 strictly less than the high key when the page is an internal page.
 *
 * - That within the page, every data item is strictly less than the item
 *	 immediately to its right, if any (i.e., that the items are in order
 *	 within the page, so that the binary searches performed by index scans are
 *	 sane).
 *
 * - That the last data item stored on the page is strictly less than the
 *	 first data item on the page to the right (when such a first item is
 *	 available).
 *
 * - Various checks on the structure of tuples themselves.  For example, check
 *	 that non-pivot tuples have no truncated attributes.
 *
 * Furthermore, when state passed shows ShareLock held, function also checks:
 *
 * - That all child pages respect strict lower bound from parent's pivot
 *	 tuple.
 *
 * - That downlink to block was encountered in parent where that's expected.
 *   (Limited to heapallindexed readonly callers.)
 *
 * This is also where heapallindexed callers use their Bloom filter to
 * fingerprint IndexTuples for later table_index_build_scan() verification.
 *
 * Note:  Memory allocated in this routine is expected to be released by caller
 * resetting state->targetcontext.
 */
static void
bt_target_page_check(BtreeCheckState *state)
{
	OffsetNumber offset;
	OffsetNumber max;
	BTPageOpaque topaque;

	topaque = (BTPageOpaque) PageGetSpecialPointer(state->target);
	max = PageGetMaxOffsetNumber(state->target);

	elog(DEBUG2, "verifying %u items on %s block %u", max,
		 P_ISLEAF(topaque) ? "leaf" : "internal", state->targetblock);

	/*
	 * Check the number of attributes in high key. Note, rightmost page
	 * doesn't contain a high key, so nothing to check
	 */
	if (!P_RIGHTMOST(topaque))
	{
		ItemId		itemid;
		IndexTuple	itup;

		/* Verify line pointer before checking tuple */
		itemid = PageGetItemIdCareful(state, state->targetblock,
									  state->target, P_HIKEY);
		if (!_bt_check_natts(state->rel, state->heapkeyspace, state->target,
							 P_HIKEY))
		{
			itup = (IndexTuple) PageGetItem(state->target, itemid);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("wrong number of high key index tuple attributes in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index block=%u natts=%u block type=%s page lsn=%X/%X.",
										state->targetblock,
										BTreeTupleGetNAtts(itup, state->rel),
										P_ISLEAF(topaque) ? "heap" : "index",
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
		}
	}

	/*
	 * Loop over page items, starting from first non-highkey item, not high
	 * key (if any).  Most tests are not performed for the "negative infinity"
	 * real item (if any).
	 */
	for (offset = P_FIRSTDATAKEY(topaque);
		 offset <= max;
		 offset = OffsetNumberNext(offset))
	{
		ItemId		itemid;
		IndexTuple	itup;
		size_t		tupsize;
		BTScanInsert skey;
		bool		lowersizelimit;

		CHECK_FOR_INTERRUPTS();

		itemid = PageGetItemIdCareful(state, state->targetblock,
									  state->target, offset);
		itup = (IndexTuple) PageGetItem(state->target, itemid);
		tupsize = IndexTupleSize(itup);

		/*
		 * lp_len should match the IndexTuple reported length exactly, since
		 * lp_len is completely redundant in indexes, and both sources of
		 * tuple length are MAXALIGN()'d.  nbtree does not use lp_len all that
		 * frequently, and is surprisingly tolerant of corrupt lp_len fields.
		 */
		if (tupsize != ItemIdGetLength(itemid))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index tuple size does not equal lp_len in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=(%u,%u) tuple size=%zu lp_len=%u page lsn=%X/%X.",
										state->targetblock, offset,
										tupsize, ItemIdGetLength(itemid),
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn),
					 errhint("This could be a torn page problem.")));

		/* Check the number of index tuple attributes */
		if (!_bt_check_natts(state->rel, state->heapkeyspace, state->target,
							 offset))
		{
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumberNoCheck(&(itup->t_tid)),
							ItemPointerGetOffsetNumberNoCheck(&(itup->t_tid)));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("wrong number of index tuple attributes in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s natts=%u points to %s tid=%s page lsn=%X/%X.",
										itid,
										BTreeTupleGetNAtts(itup, state->rel),
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
		}

		/* Fingerprint downlink blocks in heapallindexed + readonly case */
		if (state->heapallindexed && state->readonly && !P_ISLEAF(topaque))
		{
			BlockNumber childblock = BTreeInnerTupleGetDownLink(itup);

			bloom_add_element(state->downlinkfilter,
							  (unsigned char *) &childblock,
							  sizeof(BlockNumber));
		}

		/*
		 * Don't try to generate scankey using "negative infinity" item on
		 * internal pages. They are always truncated to zero attributes.
		 */
		if (offset_is_negative_infinity(topaque, offset))
			continue;

		/*
		 * Readonly callers may optionally verify that non-pivot tuples can
		 * each be found by an independent search that starts from the root
		 */
		if (state->rootdescend && P_ISLEAF(topaque) &&
			!bt_rootdescend(state, itup))
		{
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumber(&(itup->t_tid)),
							ItemPointerGetOffsetNumber(&(itup->t_tid)));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("could not find tuple using search from root page in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s points to heap tid=%s page lsn=%X/%X.",
										itid, htid,
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
		}

		/* Build insertion scankey for current page offset */
		skey = bt_mkscankey_pivotsearch(state->rel, itup);

		/*
		 * Make sure tuple size does not exceed the relevant BTREE_VERSION
		 * specific limit.
		 *
		 * BTREE_VERSION 4 (which introduced heapkeyspace rules) requisitioned
		 * a small amount of space from BTMaxItemSize() in order to ensure
		 * that suffix truncation always has enough space to add an explicit
		 * heap TID back to a tuple -- we pessimistically assume that every
		 * newly inserted tuple will eventually need to have a heap TID
		 * appended during a future leaf page split, when the tuple becomes
		 * the basis of the new high key (pivot tuple) for the leaf page.
		 *
		 * Since the reclaimed space is reserved for that purpose, we must not
		 * enforce the slightly lower limit when the extra space has been used
		 * as intended.  In other words, there is only a cross-version
		 * difference in the limit on tuple size within leaf pages.
		 *
		 * Still, we're particular about the details within BTREE_VERSION 4
		 * internal pages.  Pivot tuples may only use the extra space for its
		 * designated purpose.  Enforce the lower limit for pivot tuples when
		 * an explicit heap TID isn't actually present. (In all other cases
		 * suffix truncation is guaranteed to generate a pivot tuple that's no
		 * larger than the first right tuple provided to it by its caller.)
		 */
		lowersizelimit = skey->heapkeyspace &&
			(P_ISLEAF(topaque) || BTreeTupleGetHeapTID(itup) == NULL);
		if (tupsize > (lowersizelimit ? BTMaxItemSize(state->target) :
					   BTMaxItemSizeNoHeapTid(state->target)))
		{
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumberNoCheck(&(itup->t_tid)),
							ItemPointerGetOffsetNumberNoCheck(&(itup->t_tid)));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index row size %zu exceeds maximum for index \"%s\"",
							tupsize, RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s points to %s tid=%s page lsn=%X/%X.",
										itid,
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
		}

		/* Fingerprint leaf page tuples (those that point to the heap) */
		if (state->heapallindexed && P_ISLEAF(topaque) && !ItemIdIsDead(itemid))
		{
			IndexTuple	norm;

			norm = bt_normalize_tuple(state, itup);
			bloom_add_element(state->filter, (unsigned char *) norm,
							  IndexTupleSize(norm));
			/* Be tidy */
			if (norm != itup)
				pfree(norm);
		}

		/*
		 * * High key check *
		 *
		 * If there is a high key (if this is not the rightmost page on its
		 * entire level), check that high key actually is upper bound on all
		 * page items.
		 *
		 * We prefer to check all items against high key rather than checking
		 * just the last and trusting that the operator class obeys the
		 * transitive law (which implies that all previous items also
		 * respected the high key invariant if they pass the item order
		 * check).
		 *
		 * Ideally, we'd compare every item in the index against every other
		 * item in the index, and not trust opclass obedience of the
		 * transitive law to bridge the gap between children and their
		 * grandparents (as well as great-grandparents, and so on).  We don't
		 * go to those lengths because that would be prohibitively expensive,
		 * and probably not markedly more effective in practice.
		 *
		 * On the leaf level, we check that the key is <= the highkey.
		 * However, on non-leaf levels we check that the key is < the highkey,
		 * because the high key is "just another separator" rather than a copy
		 * of some existing key item; we expect it to be unique among all keys
		 * on the same level.  (Suffix truncation will sometimes produce a
		 * leaf highkey that is an untruncated copy of the lastleft item, but
		 * never any other item, which necessitates weakening the leaf level
		 * check to <=.)
		 *
		 * Full explanation for why a highkey is never truly a copy of another
		 * item from the same level on internal levels:
		 *
		 * While the new left page's high key is copied from the first offset
		 * on the right page during an internal page split, that's not the
		 * full story.  In effect, internal pages are split in the middle of
		 * the firstright tuple, not between the would-be lastleft and
		 * firstright tuples: the firstright key ends up on the left side as
		 * left's new highkey, and the firstright downlink ends up on the
		 * right side as right's new "negative infinity" item.  The negative
		 * infinity tuple is truncated to zero attributes, so we're only left
		 * with the downlink.  In other words, the copying is just an
		 * implementation detail of splitting in the middle of a (pivot)
		 * tuple. (See also: "Notes About Data Representation" in the nbtree
		 * README.)
		 */
		if (!P_RIGHTMOST(topaque) &&
			!(P_ISLEAF(topaque) ? invariant_leq_offset(state, skey, P_HIKEY) :
			  invariant_l_offset(state, skey, P_HIKEY)))
		{
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumberNoCheck(&(itup->t_tid)),
							ItemPointerGetOffsetNumberNoCheck(&(itup->t_tid)));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("high key invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s points to %s tid=%s page lsn=%X/%X.",
										itid,
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
		}

		/*
		 * * Item order check *
		 *
		 * Check that items are stored on page in logical order, by checking
		 * current item is strictly less than next item (if any).
		 */
		if (OffsetNumberNext(offset) <= max &&
			!invariant_l_offset(state, skey, OffsetNumberNext(offset)))
		{
			char	   *itid,
					   *htid,
					   *nitid,
					   *nhtid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumberNoCheck(&(itup->t_tid)),
							ItemPointerGetOffsetNumberNoCheck(&(itup->t_tid)));
			nitid = psprintf("(%u,%u)", state->targetblock,
							 OffsetNumberNext(offset));

			/* Reuse itup to get pointed-to heap location of second item */
			itemid = PageGetItemIdCareful(state, state->targetblock,
										  state->target,
										  OffsetNumberNext(offset));
			itup = (IndexTuple) PageGetItem(state->target, itemid);
			nhtid = psprintf("(%u,%u)",
							 ItemPointerGetBlockNumberNoCheck(&(itup->t_tid)),
							 ItemPointerGetOffsetNumberNoCheck(&(itup->t_tid)));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("item order invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Lower index tid=%s (points to %s tid=%s) "
										"higher index tid=%s (points to %s tid=%s) "
										"page lsn=%X/%X.",
										itid,
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										nitid,
										P_ISLEAF(topaque) ? "heap" : "index",
										nhtid,
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
		}

		/*
		 * * Last item check *
		 *
		 * Check last item against next/right page's first data item's when
		 * last item on page is reached.  This additional check will detect
		 * transposed pages iff the supposed right sibling page happens to
		 * belong before target in the key space.  (Otherwise, a subsequent
		 * heap verification will probably detect the problem.)
		 *
		 * This check is similar to the item order check that will have
		 * already been performed for every other "real" item on target page
		 * when last item is checked.  The difference is that the next item
		 * (the item that is compared to target's last item) needs to come
		 * from the next/sibling page.  There may not be such an item
		 * available from sibling for various reasons, though (e.g., target is
		 * the rightmost page on level).
		 */
		else if (offset == max)
		{
			BTScanInsert rightkey;

			/* Get item in next/right page */
			rightkey = bt_right_page_check_scankey(state);

			if (rightkey &&
				!invariant_g_offset(state, rightkey, max))
			{
				/*
				 * As explained at length in bt_right_page_check_scankey(),
				 * there is a known !readonly race that could account for
				 * apparent violation of invariant, which we must check for
				 * before actually proceeding with raising error.  Our canary
				 * condition is that target page was deleted.
				 */
				if (!state->readonly)
				{
					/* Get fresh copy of target page */
					state->target = palloc_btree_page(state, state->targetblock);
					/* Note that we deliberately do not update target LSN */
					topaque = (BTPageOpaque) PageGetSpecialPointer(state->target);

					/*
					 * All !readonly checks now performed; just return
					 */
					if (P_IGNORE(topaque))
						return;
				}

				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("cross page item order invariant violated for index \"%s\"",
								RelationGetRelationName(state->rel)),
						 errdetail_internal("Last item on page tid=(%u,%u) page lsn=%X/%X.",
											state->targetblock, offset,
											(uint32) (state->targetlsn >> 32),
											(uint32) state->targetlsn)));
			}
		}

		/*
		 * * Downlink check *
		 *
		 * Additional check of child items iff this is an internal page and
		 * caller holds a ShareLock.  This happens for every downlink (item)
		 * in target excluding the negative-infinity downlink (again, this is
		 * because it has no useful value to compare).
		 */
		if (!P_ISLEAF(topaque) && state->readonly)
		{
			BlockNumber childblock = BTreeInnerTupleGetDownLink(itup);

			bt_downlink_check(state, skey, childblock);
		}
	}

	/*
	 * * Check if page has a downlink in parent *
	 *
	 * This can only be checked in heapallindexed + readonly case.
	 */
	if (state->heapallindexed && state->readonly)
		bt_downlink_missing_check(state);
}

/*
 * Return a scankey for an item on page to right of current target (or the
 * first non-ignorable page), sufficient to check ordering invariant on last
 * item in current target page.  Returned scankey relies on local memory
 * allocated for the child page, which caller cannot pfree().  Caller's memory
 * context should be reset between calls here.
 *
 * This is the first data item, and so all adjacent items are checked against
 * their immediate sibling item (which may be on a sibling page, or even a
 * "cousin" page at parent boundaries where target's rightlink points to page
 * with different parent page).  If no such valid item is available, return
 * NULL instead.
 *
 * Note that !readonly callers must reverify that target page has not
 * been concurrently deleted.
 */
static BTScanInsert
bt_right_page_check_scankey(BtreeCheckState *state)
{
	BTPageOpaque opaque;
	ItemId		rightitem;
	IndexTuple	firstitup;
	BlockNumber targetnext;
	Page		rightpage;
	OffsetNumber nline;

	/* Determine target's next block number */
	opaque = (BTPageOpaque) PageGetSpecialPointer(state->target);

	/* If target is already rightmost, no right sibling; nothing to do here */
	if (P_RIGHTMOST(opaque))
		return NULL;

	/*
	 * General notes on concurrent page splits and page deletion:
	 *
	 * Routines like _bt_search() don't require *any* page split interlock
	 * when descending the tree, including something very light like a buffer
	 * pin. That's why it's okay that we don't either.  This avoidance of any
	 * need to "couple" buffer locks is the raison d' etre of the Lehman & Yao
	 * algorithm, in fact.
	 *
	 * That leaves deletion.  A deleted page won't actually be recycled by
	 * VACUUM early enough for us to fail to at least follow its right link
	 * (or left link, or downlink) and find its sibling, because recycling
	 * does not occur until no possible index scan could land on the page.
	 * Index scans can follow links with nothing more than their snapshot as
	 * an interlock and be sure of at least that much.  (See page
	 * recycling/RecentGlobalXmin notes in nbtree README.)
	 *
	 * Furthermore, it's okay if we follow a rightlink and find a half-dead or
	 * dead (ignorable) page one or more times.  There will either be a
	 * further right link to follow that leads to a live page before too long
	 * (before passing by parent's rightmost child), or we will find the end
	 * of the entire level instead (possible when parent page is itself the
	 * rightmost on its level).
	 */
	targetnext = opaque->btpo_next;
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		rightpage = palloc_btree_page(state, targetnext);
		opaque = (BTPageOpaque) PageGetSpecialPointer(rightpage);

		if (!P_IGNORE(opaque) || P_RIGHTMOST(opaque))
			break;

		/* We landed on a deleted page, so step right to find a live page */
		targetnext = opaque->btpo_next;
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg("level %u leftmost page of index \"%s\" was found deleted or half dead",
						opaque->btpo.level, RelationGetRelationName(state->rel)),
				 errdetail_internal("Deleted page found when building scankey from right sibling.")));

		/* Be slightly more pro-active in freeing this memory, just in case */
		pfree(rightpage);
	}

	/*
	 * No ShareLock held case -- why it's safe to proceed.
	 *
	 * Problem:
	 *
	 * We must avoid false positive reports of corruption when caller treats
	 * item returned here as an upper bound on target's last item.  In
	 * general, false positives are disallowed.  Avoiding them here when
	 * caller is !readonly is subtle.
	 *
	 * A concurrent page deletion by VACUUM of the target page can result in
	 * the insertion of items on to this right sibling page that would
	 * previously have been inserted on our target page.  There might have
	 * been insertions that followed the target's downlink after it was made
	 * to point to right sibling instead of target by page deletion's first
	 * phase. The inserters insert items that would belong on target page.
	 * This race is very tight, but it's possible.  This is our only problem.
	 *
	 * Non-problems:
	 *
	 * We are not hindered by a concurrent page split of the target; we'll
	 * never land on the second half of the page anyway.  A concurrent split
	 * of the right page will also not matter, because the first data item
	 * remains the same within the left half, which we'll reliably land on. If
	 * we had to skip over ignorable/deleted pages, it cannot matter because
	 * their key space has already been atomically merged with the first
	 * non-ignorable page we eventually find (doesn't matter whether the page
	 * we eventually find is a true sibling or a cousin of target, which we go
	 * into below).
	 *
	 * Solution:
	 *
	 * Caller knows that it should reverify that target is not ignorable
	 * (half-dead or deleted) when cross-page sibling item comparison appears
	 * to indicate corruption (invariant fails).  This detects the single race
	 * condition that exists for caller.  This is correct because the
	 * continued existence of target block as non-ignorable (not half-dead or
	 * deleted) implies that target page was not merged into from the right by
	 * deletion; the key space at or after target never moved left.  Target's
	 * parent either has the same downlink to target as before, or a <
	 * downlink due to deletion at the left of target.  Target either has the
	 * same highkey as before, or a highkey < before when there is a page
	 * split. (The rightmost concurrently-split-from-target-page page will
	 * still have the same highkey as target was originally found to have,
	 * which for our purposes is equivalent to target's highkey itself never
	 * changing, since we reliably skip over
	 * concurrently-split-from-target-page pages.)
	 *
	 * In simpler terms, we allow that the key space of the target may expand
	 * left (the key space can move left on the left side of target only), but
	 * the target key space cannot expand right and get ahead of us without
	 * our detecting it.  The key space of the target cannot shrink, unless it
	 * shrinks to zero due to the deletion of the original page, our canary
	 * condition.  (To be very precise, we're a bit stricter than that because
	 * it might just have been that the target page split and only the
	 * original target page was deleted.  We can be more strict, just not more
	 * lax.)
	 *
	 * Top level tree walk caller moves on to next page (makes it the new
	 * target) following recovery from this race.  (cf.  The rationale for
	 * child/downlink verification needing a ShareLock within
	 * bt_downlink_check(), where page deletion is also the main source of
	 * trouble.)
	 *
	 * Note that it doesn't matter if right sibling page here is actually a
	 * cousin page, because in order for the key space to be readjusted in a
	 * way that causes us issues in next level up (guiding problematic
	 * concurrent insertions to the cousin from the grandparent rather than to
	 * the sibling from the parent), there'd have to be page deletion of
	 * target's parent page (affecting target's parent's downlink in target's
	 * grandparent page).  Internal page deletion only occurs when there are
	 * no child pages (they were all fully deleted), and caller is checking
	 * that the target's parent has at least one non-deleted (so
	 * non-ignorable) child: the target page.  (Note that the first phase of
	 * deletion atomically marks the page to be deleted half-dead/ignorable at
	 * the same time downlink in its parent is removed, so caller will
	 * definitely not fail to detect that this happened.)
	 *
	 * This trick is inspired by the method backward scans use for dealing
	 * with concurrent page splits; concurrent page deletion is a problem that
	 * similarly receives special consideration sometimes (it's possible that
	 * the backwards scan will re-read its "original" block after failing to
	 * find a right-link to it, having already moved in the opposite direction
	 * (right/"forwards") a few times to try to locate one).  Just like us,
	 * that happens only to determine if there was a concurrent page deletion
	 * of a reference page, and just like us if there was a page deletion of
	 * that reference page it means we can move on from caring about the
	 * reference page.  See the nbtree README for a full description of how
	 * that works.
	 */
	nline = PageGetMaxOffsetNumber(rightpage);

	/*
	 * Get first data item, if any
	 */
	if (P_ISLEAF(opaque) && nline >= P_FIRSTDATAKEY(opaque))
	{
		/* Return first data item (if any) */
		rightitem = PageGetItemIdCareful(state, targetnext, rightpage,
										 P_FIRSTDATAKEY(opaque));
	}
	else if (!P_ISLEAF(opaque) &&
			 nline >= OffsetNumberNext(P_FIRSTDATAKEY(opaque)))
	{
		/*
		 * Return first item after the internal page's "negative infinity"
		 * item
		 */
		rightitem = PageGetItemIdCareful(state, targetnext, rightpage,
										 OffsetNumberNext(P_FIRSTDATAKEY(opaque)));
	}
	else
	{
		/*
		 * No first item.  Page is probably empty leaf page, but it's also
		 * possible that it's an internal page with only a negative infinity
		 * item.
		 */
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg("%s block %u of index \"%s\" has no first data item",
						P_ISLEAF(opaque) ? "leaf" : "internal", targetnext,
						RelationGetRelationName(state->rel))));
		return NULL;
	}

	/*
	 * Return first real item scankey.  Note that this relies on right page
	 * memory remaining allocated.
	 */
	firstitup = (IndexTuple) PageGetItem(rightpage, rightitem);
	return bt_mkscankey_pivotsearch(state->rel, firstitup);
}

/*
 * Checks one of target's downlink against its child page.
 *
 * Conceptually, the target page continues to be what is checked here.  The
 * target block is still blamed in the event of finding an invariant violation.
 * The downlink insertion into the target is probably where any problem raised
 * here arises, and there is no such thing as a parent link, so doing the
 * verification this way around is much more practical.
 */
static void
bt_downlink_check(BtreeCheckState *state, BTScanInsert targetkey,
				  BlockNumber childblock)
{
	OffsetNumber offset;
	OffsetNumber maxoffset;
	Page		child;
	BTPageOpaque copaque;

	/*
	 * Caller must have ShareLock on target relation, because of
	 * considerations around page deletion by VACUUM.
	 *
	 * NB: In general, page deletion deletes the right sibling's downlink, not
	 * the downlink of the page being deleted; the deleted page's downlink is
	 * reused for its sibling.  The key space is thereby consolidated between
	 * the deleted page and its right sibling.  (We cannot delete a parent
	 * page's rightmost child unless it is the last child page, and we intend
	 * to also delete the parent itself.)
	 *
	 * If this verification happened without a ShareLock, the following race
	 * condition could cause false positives:
	 *
	 * In general, concurrent page deletion might occur, including deletion of
	 * the left sibling of the child page that is examined here.  If such a
	 * page deletion were to occur, closely followed by an insertion into the
	 * newly expanded key space of the child, a window for the false positive
	 * opens up: the stale parent/target downlink originally followed to get
	 * to the child legitimately ceases to be a lower bound on all items in
	 * the page, since the key space was concurrently expanded "left".
	 * (Insertion followed the "new" downlink for the child, not our now-stale
	 * downlink, which was concurrently physically removed in target/parent as
	 * part of deletion's first phase.)
	 *
	 * Note that while the cross-page-same-level last item check uses a trick
	 * that allows it to perform verification for !readonly callers, a similar
	 * trick seems difficult here.  The trick that that other check uses is,
	 * in essence, to lock down race conditions to those that occur due to
	 * concurrent page deletion of the target; that's a race that can be
	 * reliably detected before actually reporting corruption.
	 *
	 * On the other hand, we'd need to lock down race conditions involving
	 * deletion of child's left page, for long enough to read the child page
	 * into memory (in other words, a scheme with concurrently held buffer
	 * locks on both child and left-of-child pages).  That's unacceptable for
	 * amcheck functions on general principle, though.
	 */
	Assert(state->readonly);

	/*
	 * Verify child page has the downlink key from target page (its parent) as
	 * a lower bound; downlink must be strictly less than all keys on the
	 * page.
	 *
	 * Check all items, rather than checking just the first and trusting that
	 * the operator class obeys the transitive law.
	 */
	child = palloc_btree_page(state, childblock);
	copaque = (BTPageOpaque) PageGetSpecialPointer(child);
	maxoffset = PageGetMaxOffsetNumber(child);

	/*
	 * Since there cannot be a concurrent VACUUM operation in readonly mode,
	 * and since a page has no links within other pages (siblings and parent)
	 * once it is marked fully deleted, it should be impossible to land on a
	 * fully deleted page.
	 *
	 * It does not quite make sense to enforce that the page cannot even be
	 * half-dead, despite the fact the downlink is modified at the same stage
	 * that the child leaf page is marked half-dead.  That's incorrect because
	 * there may occasionally be multiple downlinks from a chain of pages
	 * undergoing deletion, where multiple successive calls are made to
	 * _bt_unlink_halfdead_page() by VACUUM before it can finally safely mark
	 * the leaf page as fully dead.  While _bt_mark_page_halfdead() usually
	 * removes the downlink to the leaf page that is marked half-dead, that's
	 * not guaranteed, so it's possible we'll land on a half-dead page with a
	 * downlink due to an interrupted multi-level page deletion.
	 *
	 * We go ahead with our checks if the child page is half-dead.  It's safe
	 * to do so because we do not test the child's high key, so it does not
	 * matter that the original high key will have been replaced by a dummy
	 * truncated high key within _bt_mark_page_halfdead().  All other page
	 * items are left intact on a half-dead page, so there is still something
	 * to test.
	 */
	if (P_ISDELETED(copaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("downlink to deleted page found in index \"%s\"",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Parent block=%u child block=%u parent page lsn=%X/%X.",
									state->targetblock, childblock,
									(uint32) (state->targetlsn >> 32),
									(uint32) state->targetlsn)));

	for (offset = P_FIRSTDATAKEY(copaque);
		 offset <= maxoffset;
		 offset = OffsetNumberNext(offset))
	{
		/*
		 * Skip comparison of target page key against "negative infinity"
		 * item, if any.  Checking it would indicate that it's not a strict
		 * lower bound, but that's only because of the hard-coding for
		 * negative infinity items within _bt_compare().
		 *
		 * If nbtree didn't truncate negative infinity tuples during internal
		 * page splits then we'd expect child's negative infinity key to be
		 * equal to the scankey/downlink from target/parent (it would be a
		 * "low key" in this hypothetical scenario, and so it would still need
		 * to be treated as a special case here).
		 *
		 * Negative infinity items can be thought of as a strict lower bound
		 * that works transitively, with the last non-negative-infinity pivot
		 * followed during a descent from the root as its "true" strict lower
		 * bound.  Only a small number of negative infinity items are truly
		 * negative infinity; those that are the first items of leftmost
		 * internal pages.  In more general terms, a negative infinity item is
		 * only negative infinity with respect to the subtree that the page is
		 * at the root of.
		 *
		 * See also: bt_rootdescend(), which can even detect transitive
		 * inconsistencies on cousin leaf pages.
		 */
		if (offset_is_negative_infinity(copaque, offset))
			continue;

		if (!invariant_l_nontarget_offset(state, targetkey, childblock, child,
										  offset))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("down-link lower bound invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Parent block=%u child index tid=(%u,%u) parent page lsn=%X/%X.",
										state->targetblock, childblock, offset,
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
	}

	pfree(child);
}

/*
 * Checks if page is missing a downlink that it should have.
 *
 * A page that lacks a downlink/parent may indicate corruption.  However, we
 * must account for the fact that a missing downlink can occasionally be
 * encountered in a non-corrupt index.  This can be due to an interrupted page
 * split, or an interrupted multi-level page deletion (i.e. there was a hard
 * crash or an error during a page split, or while VACUUM was deleting a
 * multi-level chain of pages).
 *
 * Note that this can only be called in readonly mode, so there is no need to
 * be concerned about concurrent page splits or page deletions.
 */
static void
bt_downlink_missing_check(BtreeCheckState *state)
{
	BTPageOpaque topaque = (BTPageOpaque) PageGetSpecialPointer(state->target);
	ItemId		itemid;
	IndexTuple	itup;
	Page		child;
	BTPageOpaque copaque;
	uint32		level;
	BlockNumber childblk;

	Assert(state->heapallindexed && state->readonly);
	Assert(!P_IGNORE(topaque));

	/* No next level up with downlinks to fingerprint from the true root */
	if (P_ISROOT(topaque))
		return;

	/*
	 * Incomplete (interrupted) page splits can account for the lack of a
	 * downlink.  Some inserting transaction should eventually complete the
	 * page split in passing, when it notices that the left sibling page is
	 * P_INCOMPLETE_SPLIT().
	 *
	 * In general, VACUUM is not prepared for there to be no downlink to a
	 * page that it deletes.  This is the main reason why the lack of a
	 * downlink can be reported as corruption here.  It's not obvious that an
	 * invalid missing downlink can result in wrong answers to queries,
	 * though, since index scans that land on the child may end up
	 * consistently moving right. The handling of concurrent page splits (and
	 * page deletions) within _bt_moveright() cannot distinguish
	 * inconsistencies that last for a moment from inconsistencies that are
	 * permanent and irrecoverable.
	 *
	 * VACUUM isn't even prepared to delete pages that have no downlink due to
	 * an incomplete page split, but it can detect and reason about that case
	 * by design, so it shouldn't be taken to indicate corruption.  See
	 * _bt_pagedel() for full details.
	 */
	if (state->rightsplit)
	{
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg("harmless interrupted page split detected in index %s",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Block=%u level=%u left sibling=%u page lsn=%X/%X.",
									state->targetblock, topaque->btpo.level,
									topaque->btpo_prev,
									(uint32) (state->targetlsn >> 32),
									(uint32) state->targetlsn)));
		return;
	}

	/* Target's downlink is typically present in parent/fingerprinted */
	if (!bloom_lacks_element(state->downlinkfilter,
							 (unsigned char *) &state->targetblock,
							 sizeof(BlockNumber)))
		return;

	/*
	 * Target is probably the "top parent" of a multi-level page deletion.
	 * We'll need to descend the subtree to make sure that descendant pages
	 * are consistent with that, though.
	 *
	 * If the target page (which must be non-ignorable) is a leaf page, then
	 * clearly it can't be the top parent.  The lack of a downlink is probably
	 * a symptom of a broad problem that could just as easily cause
	 * inconsistencies anywhere else.
	 */
	if (P_ISLEAF(topaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("leaf index block lacks downlink in index \"%s\"",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Block=%u page lsn=%X/%X.",
									state->targetblock,
									(uint32) (state->targetlsn >> 32),
									(uint32) state->targetlsn)));

	/* Descend from the target page, which is an internal page */
	elog(DEBUG1, "checking for interrupted multi-level deletion due to missing downlink in index \"%s\"",
		 RelationGetRelationName(state->rel));

	level = topaque->btpo.level;
	itemid = PageGetItemIdCareful(state, state->targetblock, state->target,
								  P_FIRSTDATAKEY(topaque));
	itup = (IndexTuple) PageGetItem(state->target, itemid);
	childblk = BTreeInnerTupleGetDownLink(itup);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		child = palloc_btree_page(state, childblk);
		copaque = (BTPageOpaque) PageGetSpecialPointer(child);

		if (P_ISLEAF(copaque))
			break;

		/* Do an extra sanity check in passing on internal pages */
		if (copaque->btpo.level != level - 1)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("downlink points to block in index \"%s\" whose level is not one level down",
									 RelationGetRelationName(state->rel)),
					 errdetail_internal("Top parent/target block=%u block pointed to=%u expected level=%u level in pointed to block=%u.",
										state->targetblock, childblk,
										level - 1, copaque->btpo.level)));

		level = copaque->btpo.level;
		itemid = PageGetItemIdCareful(state, childblk, child,
									  P_FIRSTDATAKEY(copaque));
		itup = (IndexTuple) PageGetItem(child, itemid);
		childblk = BTreeInnerTupleGetDownLink(itup);
		/* Be slightly more pro-active in freeing this memory, just in case */
		pfree(child);
	}

	/*
	 * Since there cannot be a concurrent VACUUM operation in readonly mode,
	 * and since a page has no links within other pages (siblings and parent)
	 * once it is marked fully deleted, it should be impossible to land on a
	 * fully deleted page.  See bt_downlink_check() for further details.
	 *
	 * The bt_downlink_check() P_ISDELETED() check is repeated here because
	 * bt_downlink_check() does not visit pages reachable through negative
	 * infinity items.  Besides, bt_downlink_check() is unwilling to descend
	 * multiple levels.  (The similar bt_downlink_check() P_ISDELETED() check
	 * within bt_check_level_from_leftmost() won't reach the page either,
	 * since the leaf's live siblings should have their sibling links updated
	 * to bypass the deletion target page when it is marked fully dead.)
	 *
	 * If this error is raised, it might be due to a previous multi-level page
	 * deletion that failed to realize that it wasn't yet safe to mark the
	 * leaf page as fully dead.  A "dangling downlink" will still remain when
	 * this happens.  The fact that the dangling downlink's page (the leaf's
	 * parent/ancestor page) lacked a downlink is incidental.
	 */
	if (P_ISDELETED(copaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("downlink to deleted leaf page found in index \"%s\"",
								 RelationGetRelationName(state->rel)),
				 errdetail_internal("Top parent/target block=%u leaf block=%u top parent/target lsn=%X/%X.",
									state->targetblock, childblk,
									(uint32) (state->targetlsn >> 32),
									(uint32) state->targetlsn)));

	/*
	 * Iff leaf page is half-dead, its high key top parent link should point
	 * to what VACUUM considered to be the top parent page at the instant it
	 * was interrupted.  Provided the high key link actually points to the
	 * target page, the missing downlink we detected is consistent with there
	 * having been an interrupted multi-level page deletion.  This means that
	 * the subtree with the target page at its root (a page deletion chain) is
	 * in a consistent state, enabling VACUUM to resume deleting the entire
	 * chain the next time it encounters the half-dead leaf page.
	 */
	if (P_ISHALFDEAD(copaque) && !P_RIGHTMOST(copaque))
	{
		itemid = PageGetItemIdCareful(state, childblk, child, P_HIKEY);
		itup = (IndexTuple) PageGetItem(child, itemid);
		if (BTreeTupleGetTopParent(itup) == state->targetblock)
			return;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("internal index block lacks downlink in index \"%s\"",
					RelationGetRelationName(state->rel)),
			 errdetail_internal("Block=%u level=%u page lsn=%X/%X.",
								state->targetblock, topaque->btpo.level,
								(uint32) (state->targetlsn >> 32),
								(uint32) state->targetlsn)));
}

/*
 * Per-tuple callback from table_index_build_scan, used to determine if index has
 * all the entries that definitely should have been observed in leaf pages of
 * the target index (that is, all IndexTuples that were fingerprinted by our
 * Bloom filter).  All heapallindexed checks occur here.
 *
 * The redundancy between an index and the table it indexes provides a good
 * opportunity to detect corruption, especially corruption within the table.
 * The high level principle behind the verification performed here is that any
 * IndexTuple that should be in an index following a fresh CREATE INDEX (based
 * on the same index definition) should also have been in the original,
 * existing index, which should have used exactly the same representation
 *
 * Since the overall structure of the index has already been verified, the most
 * likely explanation for error here is a corrupt heap page (could be logical
 * or physical corruption).  Index corruption may still be detected here,
 * though.  Only readonly callers will have verified that left links and right
 * links are in agreement, and so it's possible that a leaf page transposition
 * within index is actually the source of corruption detected here (for
 * !readonly callers).  The checks performed only for readonly callers might
 * more accurately frame the problem as a cross-page invariant issue (this
 * could even be due to recovery not replaying all WAL records).  The !readonly
 * ERROR message raised here includes a HINT about retrying with readonly
 * verification, just in case it's a cross-page invariant issue, though that
 * isn't particularly likely.
 *
 * table_index_build_scan() expects to be able to find the root tuple when a
 * heap-only tuple (the live tuple at the end of some HOT chain) needs to be
 * indexed, in order to replace the actual tuple's TID with the root tuple's
 * TID (which is what we're actually passed back here).  The index build heap
 * scan code will raise an error when a tuple that claims to be the root of the
 * heap-only tuple's HOT chain cannot be located.  This catches cases where the
 * original root item offset/root tuple for a HOT chain indicates (for whatever
 * reason) that the entire HOT chain is dead, despite the fact that the latest
 * heap-only tuple should be indexed.  When this happens, sequential scans may
 * always give correct answers, and all indexes may be considered structurally
 * consistent (i.e. the nbtree structural checks would not detect corruption).
 * It may be the case that only index scans give wrong answers, and yet heap or
 * SLRU corruption is the real culprit.  (While it's true that LP_DEAD bit
 * setting will probably also leave the index in a corrupt state before too
 * long, the problem is nonetheless that there is heap corruption.)
 *
 * Heap-only tuple handling within table_index_build_scan() works in a way that
 * helps us to detect index tuples that contain the wrong values (values that
 * don't match the latest tuple in the HOT chain).  This can happen when there
 * is no superseding index tuple due to a faulty assessment of HOT safety,
 * perhaps during the original CREATE INDEX.  Because the latest tuple's
 * contents are used with the root TID, an error will be raised when a tuple
 * with the same TID but non-matching attribute values is passed back to us.
 * Faulty assessment of HOT-safety was behind at least two distinct CREATE
 * INDEX CONCURRENTLY bugs that made it into stable releases, one of which was
 * undetected for many years.  In short, the same principle that allows a
 * REINDEX to repair corruption when there was an (undetected) broken HOT chain
 * also allows us to detect the corruption in many cases.
 */
static void
bt_tuple_present_callback(Relation index, HeapTuple htup, Datum *values,
						  bool *isnull, bool tupleIsAlive, void *checkstate)
{
	BtreeCheckState *state = (BtreeCheckState *) checkstate;
	IndexTuple	itup,
				norm;

	Assert(state->heapallindexed);

	/* Generate a normalized index tuple for fingerprinting */
	itup = index_form_tuple(RelationGetDescr(index), values, isnull);
	itup->t_tid = htup->t_self;
	norm = bt_normalize_tuple(state, itup);

	/* Probe Bloom filter -- tuple should be present */
	if (bloom_lacks_element(state->filter, (unsigned char *) norm,
							IndexTupleSize(norm)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" lacks matching index tuple within index \"%s\"",
						ItemPointerGetBlockNumber(&(itup->t_tid)),
						ItemPointerGetOffsetNumber(&(itup->t_tid)),
						RelationGetRelationName(state->heaprel),
						RelationGetRelationName(state->rel)),
				 !state->readonly
				 ? errhint("Retrying verification using the function bt_index_parent_check() might provide a more specific error.")
				 : 0));

	state->heaptuplespresent++;
	pfree(itup);
	/* Cannot leak memory here */
	if (norm != itup)
		pfree(norm);
}

/*
 * Normalize an index tuple for fingerprinting.
 *
 * In general, index tuple formation is assumed to be deterministic by
 * heapallindexed verification, and IndexTuples are assumed immutable.  While
 * the LP_DEAD bit is mutable in leaf pages, that's ItemId metadata, which is
 * not fingerprinted.  Normalization is required to compensate for corner
 * cases where the determinism assumption doesn't quite work.
 *
 * There is currently one such case: index_form_tuple() does not try to hide
 * the source TOAST state of input datums.  The executor applies TOAST
 * compression for heap tuples based on different criteria to the compression
 * applied within btinsert()'s call to index_form_tuple(): it sometimes
 * compresses more aggressively, resulting in compressed heap tuple datums but
 * uncompressed corresponding index tuple datums.  A subsequent heapallindexed
 * verification will get a logically equivalent though bitwise unequal tuple
 * from index_form_tuple().  False positive heapallindexed corruption reports
 * could occur without normalizing away the inconsistency.
 *
 * Returned tuple is often caller's own original tuple.  Otherwise, it is a
 * new representation of caller's original index tuple, palloc()'d in caller's
 * memory context.
 *
 * Note: This routine is not concerned with distinctions about the
 * representation of tuples beyond those that might break heapallindexed
 * verification.  In particular, it won't try to normalize opclass-equal
 * datums with potentially distinct representations (e.g., btree/numeric_ops
 * index datums will not get their display scale normalized-away here).
 * Normalization may need to be expanded to handle more cases in the future,
 * though.  For example, it's possible that non-pivot tuples could in the
 * future have alternative logically equivalent representations due to using
 * the INDEX_ALT_TID_MASK bit to implement intelligent deduplication.
 */
static IndexTuple
bt_normalize_tuple(BtreeCheckState *state, IndexTuple itup)
{
	TupleDesc	tupleDescriptor = RelationGetDescr(state->rel);
	Datum		normalized[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		need_free[INDEX_MAX_KEYS];
	bool		formnewtup = false;
	IndexTuple	reformed;
	int			i;

	/* Easy case: It's immediately clear that tuple has no varlena datums */
	if (!IndexTupleHasVarwidths(itup))
		return itup;

	for (i = 0; i < tupleDescriptor->natts; i++)
	{
		Form_pg_attribute att;

		att = TupleDescAttr(tupleDescriptor, i);

		/* Assume untoasted/already normalized datum initially */
		need_free[i] = false;
		normalized[i] = index_getattr(itup, att->attnum,
									  tupleDescriptor,
									  &isnull[i]);
		if (att->attbyval || att->attlen != -1 || isnull[i])
			continue;

		/*
		 * Callers always pass a tuple that could safely be inserted into the
		 * index without further processing, so an external varlena header
		 * should never be encountered here
		 */
		if (VARATT_IS_EXTERNAL(DatumGetPointer(normalized[i])))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("external varlena datum in tuple that references heap row (%u,%u) in index \"%s\"",
							ItemPointerGetBlockNumber(&(itup->t_tid)),
							ItemPointerGetOffsetNumber(&(itup->t_tid)),
							RelationGetRelationName(state->rel))));
		else if (!VARATT_IS_COMPRESSED(DatumGetPointer(normalized[i])) &&
				 VARSIZE(DatumGetPointer(normalized[i])) > TOAST_INDEX_TARGET &&
				 (att->attstorage == 'x' ||
				  att->attstorage == 'm'))
		{
			/*
			 * This value will be compressed by index_form_tuple() with the
			 * current storage settings.  We may be here because this tuple
			 * was formed with different storage settings.  So, force forming.
			 */
			formnewtup = true;
		}
		else if (VARATT_IS_COMPRESSED(DatumGetPointer(normalized[i])))
		{
			formnewtup = true;
			normalized[i] = PointerGetDatum(PG_DETOAST_DATUM(normalized[i]));
			need_free[i] = true;
		}

		/*
		 * Short tuples may have 1B or 4B header. Convert 4B header of short
		 * tuples to 1B
		 */
		else if (VARATT_CAN_MAKE_SHORT(DatumGetPointer(normalized[i])))
		{
			/* convert to short varlena */
			Size		len = VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(normalized[i]));
			char	   *data = palloc(len);

			SET_VARSIZE_SHORT(data, len);
			memcpy(data + 1, VARDATA(DatumGetPointer(normalized[i])), len - 1);

			formnewtup = true;
			normalized[i] = PointerGetDatum(data);
			need_free[i] = true;
		}
	}

	/*
	 * Easier case: Tuple has varlena datums, none of which are compressed or
	 * short with 4B header
	 */
	if (!formnewtup)
		return itup;

	/*
	 * Hard case: Tuple had compressed varlena datums that necessitate
	 * creating normalized version of the tuple from uncompressed input datums
	 * (normalized input datums).  This is rather naive, but shouldn't be
	 * necessary too often.
	 *
	 * In the heap, tuples may contain short varlena datums with both 1B
	 * header and 4B headers.  But the corresponding index tuple should always
	 * have such varlena's with 1B headers.  So, if there is a short varlena
	 * with 4B header, we need to convert it for for fingerprinting.
	 *
	 * Note that we rely on deterministic index_form_tuple() TOAST compression
	 * of normalized input.
	 */
	reformed = index_form_tuple(tupleDescriptor, normalized, isnull);
	reformed->t_tid = itup->t_tid;

	/* Cannot leak memory here */
	for (i = 0; i < tupleDescriptor->natts; i++)
		if (need_free[i])
			pfree(DatumGetPointer(normalized[i]));

	return reformed;
}

/*
 * Search for itup in index, starting from fast root page.  itup must be a
 * non-pivot tuple.  This is only supported with heapkeyspace indexes, since
 * we rely on having fully unique keys to find a match with only a single
 * visit to a leaf page, barring an interrupted page split, where we may have
 * to move right.  (A concurrent page split is impossible because caller must
 * be readonly caller.)
 *
 * This routine can detect very subtle transitive consistency issues across
 * more than one level of the tree.  Leaf pages all have a high key (even the
 * rightmost page has a conceptual positive infinity high key), but not a low
 * key.  Their downlink in parent is a lower bound, which along with the high
 * key is almost enough to detect every possible inconsistency.  A downlink
 * separator key value won't always be available from parent, though, because
 * the first items of internal pages are negative infinity items, truncated
 * down to zero attributes during internal page splits.  While it's true that
 * bt_downlink_check() and the high key check can detect most imaginable key
 * space problems, there are remaining problems it won't detect with non-pivot
 * tuples in cousin leaf pages.  Starting a search from the root for every
 * existing leaf tuple detects small inconsistencies in upper levels of the
 * tree that cannot be detected any other way.  (Besides all this, this is
 * probably also useful as a direct test of the code used by index scans
 * themselves.)
 */
static bool
bt_rootdescend(BtreeCheckState *state, IndexTuple itup)
{
	BTScanInsert key;
	BTStack		stack;
	Buffer		lbuf;
	bool		exists;

	key = _bt_mkscankey(state->rel, itup);
	Assert(key->heapkeyspace && key->scantid != NULL);

	/*
	 * Search from root.
	 *
	 * Ideally, we would arrange to only move right within _bt_search() when
	 * an interrupted page split is detected (i.e. when the incomplete split
	 * bit is found to be set), but for now we accept the possibility that
	 * that could conceal an inconsistency.
	 */
	Assert(state->readonly && state->rootdescend);
	exists = false;
	stack = _bt_search(state->rel, key, &lbuf, BT_READ, NULL);

	if (BufferIsValid(lbuf))
	{
		BTInsertStateData insertstate;
		OffsetNumber offnum;
		Page		page;

		insertstate.itup = itup;
		insertstate.itemsz = MAXALIGN(IndexTupleSize(itup));
		insertstate.itup_key = key;
		insertstate.bounds_valid = false;
		insertstate.buf = lbuf;

		/* Get matching tuple on leaf page */
		offnum = _bt_binsrch_insert(state->rel, &insertstate);
		/* Compare first >= matching item on leaf page, if any */
		page = BufferGetPage(lbuf);
		if (offnum <= PageGetMaxOffsetNumber(page) &&
			_bt_compare(state->rel, key, page, offnum) == 0)
			exists = true;
		_bt_relbuf(state->rel, lbuf);
	}

	_bt_freestack(stack);
	pfree(key);

	return exists;
}

/*
 * Is particular offset within page (whose special state is passed by caller)
 * the page negative-infinity item?
 *
 * As noted in comments above _bt_compare(), there is special handling of the
 * first data item as a "negative infinity" item.  The hard-coding within
 * _bt_compare() makes comparing this item for the purposes of verification
 * pointless at best, since the IndexTuple only contains a valid TID (a
 * reference TID to child page).
 */
static inline bool
offset_is_negative_infinity(BTPageOpaque opaque, OffsetNumber offset)
{
	/*
	 * For internal pages only, the first item after high key, if any, is
	 * negative infinity item.  Internal pages always have a negative infinity
	 * item, whereas leaf pages never have one.  This implies that negative
	 * infinity item is either first or second line item, or there is none
	 * within page.
	 *
	 * Negative infinity items are a special case among pivot tuples.  They
	 * always have zero attributes, while all other pivot tuples always have
	 * nkeyatts attributes.
	 *
	 * Right-most pages don't have a high key, but could be said to
	 * conceptually have a "positive infinity" high key.  Thus, there is a
	 * symmetry between down link items in parent pages, and high keys in
	 * children.  Together, they represent the part of the key space that
	 * belongs to each page in the index.  For example, all children of the
	 * root page will have negative infinity as a lower bound from root
	 * negative infinity downlink, and positive infinity as an upper bound
	 * (implicitly, from "imaginary" positive infinity high key in root).
	 */
	return !P_ISLEAF(opaque) && offset == P_FIRSTDATAKEY(opaque);
}

/*
 * Does the invariant hold that the key is strictly less than a given upper
 * bound offset item?
 *
 * Verifies line pointer on behalf of caller.
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 */
static inline bool
invariant_l_offset(BtreeCheckState *state, BTScanInsert key,
				   OffsetNumber upperbound)
{
	ItemId		itemid;
	int32		cmp;

	Assert(key->pivotsearch);

	/* Verify line pointer before checking tuple */
	itemid = PageGetItemIdCareful(state, state->targetblock, state->target,
								  upperbound);
	/* pg_upgrade'd indexes may legally have equal sibling tuples */
	if (!key->heapkeyspace)
		return invariant_leq_offset(state, key, upperbound);

	cmp = _bt_compare(state->rel, key, state->target, upperbound);

	/*
	 * _bt_compare() is capable of determining that a scankey with a
	 * filled-out attribute is greater than pivot tuples where the comparison
	 * is resolved at a truncated attribute (value of attribute in pivot is
	 * minus infinity).  However, it is not capable of determining that a
	 * scankey is _less than_ a tuple on the basis of a comparison resolved at
	 * _scankey_ minus infinity attribute.  Complete an extra step to simulate
	 * having minus infinity values for omitted scankey attribute(s).
	 */
	if (cmp == 0)
	{
		BTPageOpaque topaque;
		IndexTuple	ritup;
		int			uppnkeyatts;
		ItemPointer rheaptid;
		bool		nonpivot;

		ritup = (IndexTuple) PageGetItem(state->target, itemid);
		topaque = (BTPageOpaque) PageGetSpecialPointer(state->target);
		nonpivot = P_ISLEAF(topaque) && upperbound >= P_FIRSTDATAKEY(topaque);

		/* Get number of keys + heap TID for item to the right */
		uppnkeyatts = BTreeTupleGetNKeyAtts(ritup, state->rel);
		rheaptid = BTreeTupleGetHeapTIDCareful(state, ritup, nonpivot);

		/* Heap TID is tiebreaker key attribute */
		if (key->keysz == uppnkeyatts)
			return key->scantid == NULL && rheaptid != NULL;

		return key->keysz < uppnkeyatts;
	}

	return cmp < 0;
}

/*
 * Does the invariant hold that the key is less than or equal to a given upper
 * bound offset item?
 *
 * Caller should have verified that upperbound's line pointer is consistent
 * using PageGetItemIdCareful() call.
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 */
static inline bool
invariant_leq_offset(BtreeCheckState *state, BTScanInsert key,
					 OffsetNumber upperbound)
{
	int32		cmp;

	Assert(key->pivotsearch);

	cmp = _bt_compare(state->rel, key, state->target, upperbound);

	return cmp <= 0;
}

/*
 * Does the invariant hold that the key is strictly greater than a given lower
 * bound offset item?
 *
 * Caller should have verified that lowerbound's line pointer is consistent
 * using PageGetItemIdCareful() call.
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 */
static inline bool
invariant_g_offset(BtreeCheckState *state, BTScanInsert key,
				   OffsetNumber lowerbound)
{
	int32		cmp;

	Assert(key->pivotsearch);

	cmp = _bt_compare(state->rel, key, state->target, lowerbound);

	/* pg_upgrade'd indexes may legally have equal sibling tuples */
	if (!key->heapkeyspace)
		return cmp >= 0;

	/*
	 * No need to consider the possibility that scankey has attributes that we
	 * need to force to be interpreted as negative infinity.  _bt_compare() is
	 * able to determine that scankey is greater than negative infinity.  The
	 * distinction between "==" and "<" isn't interesting here, since
	 * corruption is indicated either way.
	 */
	return cmp > 0;
}

/*
 * Does the invariant hold that the key is strictly less than a given upper
 * bound offset item, with the offset relating to a caller-supplied page that
 * is not the current target page?
 *
 * Caller's non-target page is a child page of the target, checked as part of
 * checking a property of the target page (i.e. the key comes from the
 * target).  Verifies line pointer on behalf of caller.
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 */
static inline bool
invariant_l_nontarget_offset(BtreeCheckState *state, BTScanInsert key,
							 BlockNumber nontargetblock, Page nontarget,
							 OffsetNumber upperbound)
{
	ItemId		itemid;
	int32		cmp;

	Assert(key->pivotsearch);

	/* Verify line pointer before checking tuple */
	itemid = PageGetItemIdCareful(state, nontargetblock, nontarget,
								  upperbound);
	cmp = _bt_compare(state->rel, key, nontarget, upperbound);

	/* pg_upgrade'd indexes may legally have equal sibling tuples */
	if (!key->heapkeyspace)
		return cmp <= 0;

	/* See invariant_l_offset() for an explanation of this extra step */
	if (cmp == 0)
	{
		IndexTuple	child;
		int			uppnkeyatts;
		ItemPointer childheaptid;
		BTPageOpaque copaque;
		bool		nonpivot;

		child = (IndexTuple) PageGetItem(nontarget, itemid);
		copaque = (BTPageOpaque) PageGetSpecialPointer(nontarget);
		nonpivot = P_ISLEAF(copaque) && upperbound >= P_FIRSTDATAKEY(copaque);

		/* Get number of keys + heap TID for child/non-target item */
		uppnkeyatts = BTreeTupleGetNKeyAtts(child, state->rel);
		childheaptid = BTreeTupleGetHeapTIDCareful(state, child, nonpivot);

		/* Heap TID is tiebreaker key attribute */
		if (key->keysz == uppnkeyatts)
			return key->scantid == NULL && childheaptid != NULL;

		return key->keysz < uppnkeyatts;
	}

	return cmp < 0;
}

/*
 * Given a block number of a B-Tree page, return page in palloc()'d memory.
 * While at it, perform some basic checks of the page.
 *
 * There is never an attempt to get a consistent view of multiple pages using
 * multiple concurrent buffer locks; in general, we only acquire a single pin
 * and buffer lock at a time, which is often all that the nbtree code requires.
 *
 * Operating on a copy of the page is useful because it prevents control
 * getting stuck in an uninterruptible state when an underlying operator class
 * misbehaves.
 */
static Page
palloc_btree_page(BtreeCheckState *state, BlockNumber blocknum)
{
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber maxoffset;

	page = palloc(BLCKSZ);

	/*
	 * We copy the page into local storage to avoid holding pin on the buffer
	 * longer than we must.
	 */
	buffer = ReadBufferExtended(state->rel, MAIN_FORKNUM, blocknum, RBM_NORMAL,
								state->checkstrategy);
	LockBuffer(buffer, BT_READ);

	/*
	 * Perform the same basic sanity checking that nbtree itself performs for
	 * every page:
	 */
	_bt_checkpage(state->rel, buffer);

	/* Only use copy of page in palloc()'d memory */
	memcpy(page, BufferGetPage(buffer), BLCKSZ);
	UnlockReleaseBuffer(buffer);

	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	if (P_ISMETA(opaque) && blocknum != BTREE_METAPAGE)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid meta page found at block %u in index \"%s\"",
						blocknum, RelationGetRelationName(state->rel))));

	/* Check page from block that ought to be meta page */
	if (blocknum == BTREE_METAPAGE)
	{
		BTMetaPageData *metad = BTPageGetMeta(page);

		if (!P_ISMETA(opaque) ||
			metad->btm_magic != BTREE_MAGIC)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" meta page is corrupt",
							RelationGetRelationName(state->rel))));

		if (metad->btm_version < BTREE_MIN_VERSION ||
			metad->btm_version > BTREE_VERSION)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("version mismatch in index \"%s\": file version %d, "
							"current version %d, minimum supported version %d",
							RelationGetRelationName(state->rel),
							metad->btm_version, BTREE_VERSION,
							BTREE_MIN_VERSION)));

		/* Finished with metapage checks */
		return page;
	}

	/*
	 * Deleted pages have no sane "level" field, so can only check non-deleted
	 * page level
	 */
	if (P_ISLEAF(opaque) && !P_ISDELETED(opaque) && opaque->btpo.level != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid leaf page level %u for block %u in index \"%s\"",
						opaque->btpo.level, blocknum, RelationGetRelationName(state->rel))));

	if (!P_ISLEAF(opaque) && !P_ISDELETED(opaque) &&
		opaque->btpo.level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid internal page level 0 for block %u in index \"%s\"",
						blocknum, RelationGetRelationName(state->rel))));

	/*
	 * Sanity checks for number of items on page.
	 *
	 * As noted at the beginning of _bt_binsrch(), an internal page must have
	 * children, since there must always be a negative infinity downlink
	 * (there may also be a highkey).  In the case of non-rightmost leaf
	 * pages, there must be at least a highkey.  Deleted pages on replica
	 * might contain no items, because page unlink re-initializes
	 * page-to-be-deleted.  Deleted pages with no items might be on primary
	 * too due to preceding recovery, but on primary new deletions can't
	 * happen concurrently to amcheck.
	 *
	 * This is correct when pages are half-dead, since internal pages are
	 * never half-dead, and leaf pages must have a high key when half-dead
	 * (the rightmost page can never be deleted).  It's also correct with
	 * fully deleted pages: _bt_unlink_halfdead_page() doesn't change anything
	 * about the target page other than setting the page as fully dead, and
	 * setting its xact field.  In particular, it doesn't change the sibling
	 * links in the deletion target itself, since they're required when index
	 * scans land on the deletion target, and then need to move right (or need
	 * to move left, in the case of backward index scans).
	 */
	maxoffset = PageGetMaxOffsetNumber(page);
	if (maxoffset > MaxIndexTuplesPerPage)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("Number of items on block %u of index \"%s\" exceeds MaxIndexTuplesPerPage (%u)",
						blocknum, RelationGetRelationName(state->rel),
						MaxIndexTuplesPerPage)));

	if (!P_ISLEAF(opaque) && !P_ISDELETED(opaque) && maxoffset < P_FIRSTDATAKEY(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("internal block %u in index \"%s\" lacks high key and/or at least one downlink",
						blocknum, RelationGetRelationName(state->rel))));

	if (P_ISLEAF(opaque) && !P_ISDELETED(opaque) && !P_RIGHTMOST(opaque) && maxoffset < P_HIKEY)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("non-rightmost leaf block %u in index \"%s\" lacks high key item",
						blocknum, RelationGetRelationName(state->rel))));

	/*
	 * In general, internal pages are never marked half-dead, except on
	 * versions of Postgres prior to 9.4, where it can be valid transient
	 * state.  This state is nonetheless treated as corruption by VACUUM on
	 * from version 9.4 on, so do the same here.  See _bt_pagedel() for full
	 * details.
	 *
	 * Internal pages should never have garbage items, either.
	 */
	if (!P_ISLEAF(opaque) && P_ISHALFDEAD(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("internal page block %u in index \"%s\" is half-dead",
						blocknum, RelationGetRelationName(state->rel)),
				 errhint("This can be caused by an interrupted VACUUM in version 9.3 or older, before upgrade. Please REINDEX it.")));

	if (!P_ISLEAF(opaque) && P_HAS_GARBAGE(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("internal page block %u in index \"%s\" has garbage items",
						blocknum, RelationGetRelationName(state->rel))));

	return page;
}

/*
 * _bt_mkscankey() wrapper that automatically prevents insertion scankey from
 * being considered greater than the pivot tuple that its values originated
 * from (or some other identical pivot tuple) in the common case where there
 * are truncated/minus infinity attributes.  Without this extra step, there
 * are forms of corruption that amcheck could theoretically fail to report.
 *
 * For example, invariant_g_offset() might miss a cross-page invariant failure
 * on an internal level if the scankey built from the first item on the
 * target's right sibling page happened to be equal to (not greater than) the
 * last item on target page.  The !pivotsearch tiebreaker in _bt_compare()
 * might otherwise cause amcheck to assume (rather than actually verify) that
 * the scankey is greater.
 */
static inline BTScanInsert
bt_mkscankey_pivotsearch(Relation rel, IndexTuple itup)
{
	BTScanInsert skey;

	skey = _bt_mkscankey(rel, itup);
	skey->pivotsearch = true;

	return skey;
}

/*
 * PageGetItemId() wrapper that validates returned line pointer.
 *
 * Buffer page/page item access macros generally trust that line pointers are
 * not corrupt, which might cause problems for verification itself.  For
 * example, there is no bounds checking in PageGetItem().  Passing it a
 * corrupt line pointer can cause it to return a tuple/pointer that is unsafe
 * to dereference.
 *
 * Validating line pointers before tuples avoids undefined behavior and
 * assertion failures with corrupt indexes, making the verification process
 * more robust and predictable.
 */
static ItemId
PageGetItemIdCareful(BtreeCheckState *state, BlockNumber block, Page page,
					 OffsetNumber offset)
{
	ItemId		itemid = PageGetItemId(page, offset);

	if (ItemIdGetOffset(itemid) + ItemIdGetLength(itemid) >
		BLCKSZ - sizeof(BTPageOpaqueData))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("line pointer points past end of tuple space in index \"%s\"",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	/*
	 * Verify that line pointer isn't LP_REDIRECT or LP_UNUSED, since nbtree
	 * never uses either.  Verify that line pointer has storage, too, since
	 * even LP_DEAD items should within nbtree.
	 */
	if (ItemIdIsRedirected(itemid) || !ItemIdIsUsed(itemid) ||
		ItemIdGetLength(itemid) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid line pointer storage in index \"%s\"",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	return itemid;
}

/*
 * BTreeTupleGetHeapTID() wrapper that lets caller enforce that a heap TID must
 * be present in cases where that is mandatory.
 *
 * This doesn't add much as of BTREE_VERSION 4, since the INDEX_ALT_TID_MASK
 * bit is effectively a proxy for whether or not the tuple is a pivot tuple.
 * It may become more useful in the future, when non-pivot tuples support their
 * own alternative INDEX_ALT_TID_MASK representation.
 */
static inline ItemPointer
BTreeTupleGetHeapTIDCareful(BtreeCheckState *state, IndexTuple itup,
							bool nonpivot)
{
	ItemPointer result = BTreeTupleGetHeapTID(itup);
	BlockNumber targetblock = state->targetblock;

	if (result == NULL && nonpivot)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("block %u or its right sibling block or child block in index \"%s\" contains non-pivot tuple that lacks a heap TID",
						targetblock, RelationGetRelationName(state->rel))));

	return result;
}
