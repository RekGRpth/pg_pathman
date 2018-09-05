/* ------------------------------------------------------------------------
 *
 * partition_router.c
 *		Route row to a right partition in UPDATE operation
 *
 * Copyright (c) 2017, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */

#include "partition_filter.h"
#include "partition_router.h"
#include "compat/pg_compat.h"

#include "access/xact.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "commands/trigger.h"
#include "executor/nodeModifyTable.h"
#include "foreign/fdwapi.h"
#include "storage/bufmgr.h"
#include "utils/guc.h"
#include "utils/rel.h"


#define MTHackField(mt_state, field) ( (mt_state)->field )


bool				pg_pathman_enable_partition_router = true;

CustomScanMethods	partition_router_plan_methods;
CustomExecMethods	partition_router_exec_methods;


/* FIXME: replace this magic with a CustomScan */
static ExecProcNodeMtd mt_method = NULL;


static TupleTableSlot *router_run_modify_table(PlanState *state);

static TupleTableSlot *router_set_slot(PartitionRouterState *state,
									   TupleTableSlot *slot,
									   CmdType operation);
static TupleTableSlot *router_get_slot(PartitionRouterState *state,
									   bool *should_process);

static void router_lazy_init_junkfilter(PartitionRouterState *state, EState *estate);
static void router_lazy_init_constraint(PartitionRouterState *state);

static ItemPointerData router_extract_ctid(PartitionRouterState *state,
										   TupleTableSlot *slot);

static TupleTableSlot *router_lock_or_delete_tuple(PartitionRouterState *state,
												   TupleTableSlot *slot,
												   ItemPointer tupleid,
												   bool *deleted,
												   EState *estate);

void
init_partition_router_static_data(void)
{
	partition_router_plan_methods.CustomName 			= UPDATE_NODE_NAME;
	partition_router_plan_methods.CreateCustomScanState	= partition_router_create_scan_state;

	partition_router_exec_methods.CustomName			= UPDATE_NODE_NAME;
	partition_router_exec_methods.BeginCustomScan		= partition_router_begin;
	partition_router_exec_methods.ExecCustomScan		= partition_router_exec;
	partition_router_exec_methods.EndCustomScan			= partition_router_end;
	partition_router_exec_methods.ReScanCustomScan		= partition_router_rescan;
	partition_router_exec_methods.MarkPosCustomScan		= NULL;
	partition_router_exec_methods.RestrPosCustomScan	= NULL;
	partition_router_exec_methods.ExplainCustomScan		= partition_router_explain;

	DefineCustomBoolVariable("pg_pathman.enable_partitionrouter",
							 "Enables the planner's use of " UPDATE_NODE_NAME " custom node.",
							 NULL,
							 &pg_pathman_enable_partition_router,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	RegisterCustomScanMethods(&partition_router_plan_methods);
}

Plan *
make_partition_router(Plan *subplan,
					  Oid parent_relid,
					  Index parent_rti,
					  int epq_param,
					  List *returning_list)

{
	CustomScan *cscan = makeNode(CustomScan);

	/* Copy costs etc */
	cscan->scan.plan.startup_cost	= subplan->startup_cost;
	cscan->scan.plan.total_cost		= subplan->total_cost;
	cscan->scan.plan.plan_rows		= subplan->plan_rows;
	cscan->scan.plan.plan_width		= subplan->plan_width;

	/* Setup methods, child plan and param number for EPQ */
	cscan->methods = &partition_router_plan_methods;
	cscan->custom_plans = list_make1(subplan);
	cscan->custom_private = list_make1(makeInteger(epq_param));

	/* No physical relation will be scanned */
	cscan->scan.scanrelid = 0;

	/* Build an appropriate target list */
	cscan->scan.plan.targetlist = pfilter_build_tlist(subplan);

	/* FIXME: should we use the same tlist? */
	cscan->custom_scan_tlist = subplan->targetlist;

	return &cscan->scan.plan;
}

void
prepare_modify_table_for_partition_router(PlanState *state, void *context)
{
	if (IsA(state, ModifyTableState))
	{
		ModifyTableState   *mt_state = (ModifyTableState *) state;
		bool				changed_method = false;
		int					i;

		for (i = 0; i < mt_state->mt_nplans; i++)
		{
			CustomScanState		   *pf_state = (CustomScanState *) mt_state->mt_plans[i];
			PartitionRouterState   *pr_state;

			/* Check if this is a PartitionFilter + PartitionRouter combo */
			if (IsPartitionFilterState(pf_state) &&
				IsPartitionRouterState(pr_state = linitial(pf_state->custom_ps)))
			{
				/* HACK: point to ModifyTable in PartitionRouter */
				pr_state->mt_state = mt_state;

				if (!changed_method)
				{
					if (!mt_method)
						mt_method = state->ExecProcNodeReal;

					/* HACK: replace ModifyTable's execution method */
					ExecSetExecProcNode(state, router_run_modify_table);

					changed_method = true;
				}
			}
		}
	}
}


Node *
partition_router_create_scan_state(CustomScan *node)
{
	PartitionRouterState   *state;

	state = (PartitionRouterState *) palloc0(sizeof(PartitionRouterState));
	NodeSetTag(state, T_CustomScanState);

	state->css.flags = node->flags;
	state->css.methods = &partition_router_exec_methods;

	/* Extract necessary variables */
	state->epqparam = intVal(linitial(node->custom_private));
	state->subplan = (Plan *) linitial(node->custom_plans);

	return (Node *) state;
}

void
partition_router_begin(CustomScanState *node, EState *estate, int eflags)
{
	PartitionRouterState   *state = (PartitionRouterState *) node;

	/* Remember current relation we're going to delete from */
	state->current_rri = estate->es_result_relation_info;

	EvalPlanQualInit(&state->epqstate, estate,
					 state->subplan, NIL,
					 state->epqparam);

	/* It's convenient to store PlanState in 'custom_ps' */
	node->custom_ps = list_make1(ExecInitNode(state->subplan, estate, eflags));
}

TupleTableSlot *
partition_router_exec(CustomScanState *node)
{
	EState				   *estate = node->ss.ps.state;
	PartitionRouterState   *state = (PartitionRouterState *) node;
	TupleTableSlot		   *slot;
	bool					should_process;

take_next_tuple:
	/* Get next tuple for processing */
	slot = router_get_slot(state, &should_process);

	if (should_process)
	{
		CmdType				new_cmd;
		bool				deleted;
		ItemPointerData		ctid;

		ItemPointerSetInvalid(&ctid);

		/* Build new junkfilter lazily */
		router_lazy_init_junkfilter(state, estate);

		/* Build recheck constraint state lazily */
		router_lazy_init_constraint(state);

		/* Extract item pointer from current tuple */
		ctid = router_extract_ctid(state, slot);

		/* Magic: replace parent's ResultRelInfo with ours */
		estate->es_result_relation_info = state->current_rri;

		/* Delete tuple from old partition */
		Assert(ItemPointerIsValid(&ctid));
		slot = router_lock_or_delete_tuple(state, slot, &ctid,
										   &deleted, estate);

		/* We require a tuple */
		if (TupIsNull(slot))
			goto take_next_tuple;

		/* Should we use UPDATE or DELETE + INSERT? */
		new_cmd = deleted ? CMD_INSERT : CMD_UPDATE;

		/* Alter ModifyTable's state and return */
		return router_set_slot(state, slot, new_cmd);
	}

	return slot;
}

void
partition_router_end(CustomScanState *node)
{
	PartitionRouterState *state = (PartitionRouterState *) node;

	Assert(list_length(node->custom_ps) == 1);
	ExecEndNode((PlanState *) linitial(node->custom_ps));

	EvalPlanQualEnd(&state->epqstate);
}

void
partition_router_rescan(CustomScanState *node)
{
	PartitionRouterState *state = (PartitionRouterState *) node;

	Assert(list_length(node->custom_ps) == 1);
	ExecReScan((PlanState *) linitial(node->custom_ps));

	state->saved_slot = NULL;
}

void
partition_router_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	/* Nothing to do here now */
}


static TupleTableSlot *
router_run_modify_table(PlanState *state)
{
	ModifyTableState   *mt_state;
	TupleTableSlot	   *slot;
	int					mt_plans_old,
						mt_plans_new;

	mt_state = (ModifyTableState *) state;

	mt_plans_old = MTHackField(mt_state, mt_nplans);

	/* Fetch next tuple */
	slot = mt_method(state);

	mt_plans_new = MTHackField(mt_state, mt_nplans);

	/* PartitionRouter asked us to restart */
	if (mt_plans_new != mt_plans_old)
	{
		int state_idx = -mt_plans_new;

		/* HACK: partially restore ModifyTable's state */
		MTHackField(mt_state, mt_done) = false;
		MTHackField(mt_state, mt_nplans) = mt_plans_old;
		MTHackField(mt_state, mt_whichplan) = state_idx;

		/* Restart ModifyTable */
		return mt_method(state);
	}

	return slot;
}

static TupleTableSlot *
router_set_slot(PartitionRouterState *state,
				TupleTableSlot *slot,
				CmdType operation)
{
	ModifyTableState *mt_state = state->mt_state;

	/* Check invariants */
	Assert(!TupIsNull(slot));
	Assert(state->junkfilter);

	if (mt_state->operation == operation)
		return slot;

	/* HACK: alter ModifyTable's state */
	MTHackField(mt_state, mt_nplans) = -mt_state->mt_whichplan;
	MTHackField(mt_state, operation) = operation;

	/* HACK: conditionally disable junk filter in result relation */
	state->current_rri->ri_junkFilter = (operation == CMD_UPDATE) ?
											state->junkfilter :
											NULL;

	/* Set saved_slot and yield */
	state->saved_slot = slot;
	return NULL;
}

static TupleTableSlot *
router_get_slot(PartitionRouterState *state,
				bool *should_process)
{
	TupleTableSlot *slot;

	if (!TupIsNull(state->saved_slot))
	{
		/* Reset saved_slot */
		slot = state->saved_slot;
		state->saved_slot = NULL;

		/* We shouldn't process preserved slot... */
		*should_process = false;
	}
	else
	{
		slot = ExecProcNode((PlanState *) linitial(state->css.custom_ps));

		/* But we have to process non-empty slot */
		*should_process = !TupIsNull(slot);
	}

	return slot;
}

static void
router_lazy_init_junkfilter(PartitionRouterState *state, EState *estate)
{
	if (state->junkfilter == NULL)
		state->junkfilter = state->current_rri->ri_junkFilter;
}

static void
router_lazy_init_constraint(PartitionRouterState *state)
{
	Relation rel = state->current_rri->ri_RelationDesc;

	if (state->constraint == NULL)
	{
		Expr *expr = get_partition_constraint_expr(RelationGetRelid(rel));
		state->constraint = ExecInitExpr(expr, NULL);
	}
}

/* Extract ItemPointer from tuple using JunkFilter */
static ItemPointerData
router_extract_ctid(PartitionRouterState *state, TupleTableSlot *slot)
{
	Relation	rel = state->current_rri->ri_RelationDesc;
	char		relkind = RelationGetForm(rel)->relkind;

	if (relkind == RELKIND_RELATION)
	{
		Datum	ctid_datum;
		bool	ctid_isnull;

		ctid_datum = ExecGetJunkAttribute(slot,
										  state->junkfilter->jf_junkAttNo,
										  &ctid_isnull);

		/* shouldn't ever get a null result... */
		if (ctid_isnull)
			elog(ERROR, "ctid is NULL");

		/* Get item pointer to tuple */
		return *(ItemPointer) DatumGetPointer(ctid_datum);
	}
	else if (relkind == RELKIND_FOREIGN_TABLE)
		elog(ERROR, UPDATE_NODE_NAME " does not support foreign tables");
	else
		elog(ERROR, UPDATE_NODE_NAME " cannot handle relkind %u", relkind);
}

/* This is a heavily modified copy of ExecDelete from nodeModifyTable.c */
static TupleTableSlot *
router_lock_or_delete_tuple(PartitionRouterState *state,
							TupleTableSlot *slot,
							ItemPointer tupleid,
							bool *deleted,	/* return value #1 */
							EState *estate)
{
	ResultRelInfo		   *rri;
	Relation				rel;

	ExprContext			   *econtext = GetPerTupleExprContext(estate);
	ExprState			   *constraint = state->constraint;

	HeapUpdateFailureData	hufd;
	HTSU_Result				result;
	EPQState			   *epqstate = &state->epqstate;

	LOCKMODE				lockmode;
	bool					try_delete;

	*deleted = false;

	EvalPlanQualSetSlot(epqstate, slot);

	/* Get information on the (current) result relation */
	rri = estate->es_result_relation_info;
	rel = rri->ri_RelationDesc;
	lockmode = ExecUpdateLockMode(estate, rri);

recheck:
	/* Does tuple still belong to current partition? */
	econtext->ecxt_scantuple = slot;
	try_delete = !ExecCheck(constraint, econtext);

	/* Lock or delete tuple */
	if (try_delete)
	{
		/* BEFORE ROW UPDATE triggers */
		if (rri->ri_TrigDesc &&
			rri->ri_TrigDesc->trig_update_before_row)
		{
			slot = ExecBRUpdateTriggers(estate, epqstate, rri, tupleid, NULL, slot);
			if (TupIsNull(slot))
				return NULL;
		}

		/* BEFORE ROW DELETE triggers */
		if (rri->ri_TrigDesc &&
			rri->ri_TrigDesc->trig_delete_before_row)
		{
			if (!ExecBRDeleteTriggersCompat(estate, epqstate, rri, tupleid, NULL, NULL))
				return NULL;
		}

		/* Delete the tuple */
		result = heap_delete_compat(rel, tupleid,
									estate->es_output_cid,
									estate->es_crosscheck_snapshot,
									true /* wait for commit */, &hufd,
									true /* changing partition */);
	}
	else
	{
		HeapTupleData	tuple;
		Buffer			buffer;

		tuple.t_self = *tupleid;
		result = heap_lock_tuple(rel, &tuple,
								 estate->es_output_cid,
								 lockmode, LockWaitBlock,
								 false, &buffer, &hufd);

		ReleaseBuffer(buffer);
	}

	/* Check lock/delete status */
	switch (result)
	{
		case HeapTupleSelfUpdated:
			if (hufd.cmax != estate->es_output_cid)
				ereport(ERROR,
						(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
						 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
						 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

			/* Already deleted by self; nothing to do */
			return NULL;

		case HeapTupleMayBeUpdated:
			break;

		case HeapTupleUpdated:
			if (IsolationUsesXactSnapshot())
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to concurrent update")));

			if (!ItemPointerEquals(tupleid, &hufd.ctid))
			{
				TupleTableSlot *epqslot;

				epqslot = EvalPlanQual(estate,
									   epqstate,
									   rel,
									   rri->ri_RangeTableIndex,
									   LockTupleExclusive,
									   &hufd.ctid,
									   hufd.xmax);

				if (!TupIsNull(epqslot))
				{
					Assert(tupleid != NULL);
					*tupleid = hufd.ctid;
					slot = epqslot;
					goto recheck;
				}
			}

			/* Tuple already deleted; nothing to do */
			return NULL;

		case HeapTupleInvisible:
			elog(ERROR, "attempted to lock invisible tuple");
			break;

		default:
			elog(ERROR, "unrecognized heap_delete status: %u", result);
			break;
	}

	/* Additional work for delete s*/
	if (try_delete)
	{
		/* AFTER ROW DELETE triggers */
		ExecARDeleteTriggersCompat(estate, rri, tupleid, NULL, NULL);
	}

	*deleted = try_delete;
	return slot;
}