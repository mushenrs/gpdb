/*-------------------------------------------------------------------------
 *
 * nodeWorktablescan.c
 *	  routines to handle WorkTableScan nodes.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeWorktablescan.c,v 1.4 2008/10/28 17:13:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeWorktablescan.h"

static TupleTableSlot *WorkTableScanNext(WorkTableScanState *node);

/* ----------------------------------------------------------------
 *		WorkTableScanNext
 *
 *		This is a workhorse for ExecWorkTableScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
WorkTableScanNext(WorkTableScanState *node)
{
	TupleTableSlot *slot;
	EState	   *estate;
	Tuplestorestate *tuplestorestate;

	/*
	 * get information from the estate and scan state
	 *
	 * Note: we intentionally do not support backward scan.  Although it would
	 * take only a couple more lines here, it would force nodeRecursiveunion.c
	 * to create the tuplestore with backward scan enabled, which has a
	 * performance cost.  In practice backward scan is never useful for a
	 * worktable plan node, since it cannot appear high enough in the plan
	 * tree of a scrollable cursor to be exposed to a backward-scan
	 * requirement.  So it's not worth expending effort to support it.
	 */
	estate = node->ss.ps.state;

	/*
	 * GPDB_84_MERGE_FIXME: Double check we don't have backward scan required by
	 * plan (both planner and ORCA).
	 */
	Assert(ScanDirectionIsForward(estate->es_direction));

	tuplestorestate = node->rustate->working_table;

	/*
	 * Get the next tuple from tuplestore. Return NULL if no more tuples.
	 */
	slot = node->ss.ss_ScanTupleSlot;
	(void) tuplestore_gettupleslot(tuplestorestate, true, false, slot);
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecWorkTableScan(node)
 *
 *		Scans the worktable sequentially and returns the next qualifying tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieves tuples sequentially.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecWorkTableScan(WorkTableScanState *node)
{
	/*
	 * On the first call, find the ancestor RecursiveUnion's state
	 * via the Param slot reserved for it.  (We can't do this during node
	 * init because there are corner cases where we'll get the init call
	 * before the RecursiveUnion does.)
	 */
	if (node->rustate == NULL)
	{
		WorkTableScan *plan = (WorkTableScan *) node->ss.ps.plan;
		EState	   *estate = node->ss.ps.state;
		ParamExecData *param;

		param = &(estate->es_param_exec_vals[plan->wtParam]);
		Assert(param->execPlan == NULL);
		Assert(!param->isnull);
		node->rustate = (RecursiveUnionState *) DatumGetPointer(param->value);
		Assert(node->rustate && IsA(node->rustate, RecursiveUnionState));

		/*
		 * The scan tuple type (ie, the rowtype we expect to find in the work
		 * table) is the same as the result rowtype of the ancestor
		 * RecursiveUnion node.  Note this depends on the assumption that
		 * RecursiveUnion doesn't allow projection.
		 */
		ExecAssignScanType(&node->ss,
						   ExecGetResultType(&node->rustate->ps));

		/*
		 * Now we can initialize the projection info.  This must be
		 * completed before we can call ExecScan().
		 */
		ExecAssignScanProjectionInfo(&node->ss);
	}

	/*
	 * use WorkTableScanNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) WorkTableScanNext);
}


/* ----------------------------------------------------------------
 *		ExecInitWorkTableScan
 * ----------------------------------------------------------------
 */
WorkTableScanState *
ExecInitWorkTableScan(WorkTableScan *node, EState *estate, int eflags)
{
	WorkTableScanState *scanstate;

	/* check for unsupported flags */
	/*
	 * GPDB_84_MERGE_FIXME: Make sure we don't require EXEC_FLAG_BACKWARD
	 * in GPDB.
	 */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * WorkTableScan should not have any children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create new WorkTableScanState for node
	 */
	scanstate = makeNode(WorkTableScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->rustate = NULL;	/* we'll set this later */

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) scanstate);

#define WORKTABLESCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

	/*
	 * Initialize result tuple type, but not yet projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);

	/* scanstate->ss.ps.ps_TupFromTlist = false; */

	return scanstate;
}

int
ExecCountSlotsWorkTableScan(WorkTableScan *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		WORKTABLESCAN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndWorkTableScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndWorkTableScan(WorkTableScanState *node)
{
	/*
	 * Free exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecWorkTableScanReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecWorkTableScanReScan(WorkTableScanState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	/* node->ss.ps.ps_TupFromTlist = false; */

	/* No need (or way) to rescan if ExecWorkTableScan not called yet */
	if (node->rustate)
		tuplestore_rescan(node->rustate->working_table);
}
