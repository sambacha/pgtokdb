/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "pgtokdb.h"

#include <access/htup_details.h>
#include <catalog/pg_proc.h>
#include <fmgr.h>
#include <funcapi.h>
#include <utils/guc.h>
#include <utils/syscache.h>

PG_MODULE_MAGIC;

/* Configuration globals */
static char host[256] = "localhost";
static int port = 5000;
static char userpass[256] = "";

/* Prototypes */
void _PG_init(void);
int findOID(int);
int findName(char *, K);
void safecpy(char *, const char *, size_t);
K kk(I, char *, K);
void getset_init(FunctionCallInfo);
K getset_args(FunctionCallInfo);

/* Information needed across calls and stored in the function context */
typedef struct {
  K table;   /* Table result of call to kdb+ */
  int *perm; /* Permutation order that map kdb+ columns with result columns */
  int *todtind;   /* Indices into typo-oid dispatch table */
  Datum *dvalues; /* Datum for each column in the result */
  bool *nulls;    /* Null indicator (always false) for each column */
} UIFC;           /* User Information Function Context */

/* Type OID dispatch table used to determine conversion functions */
struct {
  int typeoid;                    /* OID of Postgres type */
  PGEntry (*k2p)(K, int, char *); /* Function to convert kdb+ to Postgres */
  K (*p2k)(Datum);                /* Function to convert Postgres to kdb+ */
  bool isref; /* Indicates whether Postgres Datum is a reference */
} todt[17] = {
    {BOOLOID, k2p_bool, p2k_bool, false},
    {INT2OID, k2p_int2, p2k_int2, false},
    {INT4OID, k2p_int4, p2k_int4, false},
    {INT8OID, k2p_int8, p2k_int8, false},
    {FLOAT4OID, k2p_float4, p2k_float4, false},
    {FLOAT8OID, k2p_float8, p2k_float8, false},
    {BPCHAROID, k2p_char, p2k_char, true},
    {TIMESTAMPOID, k2p_timestamp, p2k_timestamp, false},
    {TIMESTAMPTZOID, k2p_timestamp, p2k_timestamp, false},
    {VARCHAROID, k2p_varchar, p2k_varchar, true},
    {DATEOID, k2p_date, p2k_date, false},
    {UUIDOID, k2p_uuid, p2k_uuid, true},
    {TEXTOID, k2p_varchar, p2k_varchar, true},
    {BYTEAOID, k2p_bytea, p2k_bytea, true},
    {INT2ARRAYOID, k2p_int2array, NULL, true},
    {INT4ARRAYOID, k2p_int4array, NULL, true},
    // { INT8ARRAYOID,		k2p_int8array,		NULL,			true
    // },
    {FLOAT4ARRAYOID, k2p_float4array, NULL, true},
    // { FLOAT8ARRAYOID,	k2p_float8array, 	NULL,			true
    // }
    /* ... add support for additional data types here ... */
};

/*
 * Adding support for a new Postgres type requires the following:
 *	- Add a new entry in dispatch table above
 *	- Write conversion functions to map between kdb+ and Postgres (both
 *ways)
 *	- Write test cases for happy and exception paths
 *	- Update README.md file and perhaps show an additional sample
 */

/*
 * Return position of type OID in type dispatch table
 */
int findOID(int oid) {
  for (int i = 0; i < sizeof(todt) / sizeof(todt[0]); i++)
    if (todt[i].typeoid == oid) return i;
  return -1;
}

/*
 * Return position of name in kdb+ column name vector
 */
int findName(char *f, K nv) {
  for (int i = 0; i < nv->n; i++)
    if (strcmp(f, kS(nv)[i]) == 0) return i;
  return -1;
}

/*
 * A version of strncpy that is safe and always 0-terminates the string
 */
void safecpy(char *dest, const char *src, size_t n) {
  strncpy(dest, src, n);
  dest[n - 1] = '\0';
}

/*
 * Initialize configuration variables upon loading of this extension (once only)
 */
void _PG_init(void) {
  const char *p;

  if ((p = GetConfigOption("pgtokdb.host", true, false)) != NULL)
    safecpy(host, p, sizeof(host));

  if ((p = GetConfigOption("pgtokdb.port", true, false)) != NULL)
    port = atoi(p);

  if ((p = GetConfigOption("pgtokdb.userpass", true, false)) != NULL)
    safecpy(userpass, p, sizeof(userpass));
}

PG_FUNCTION_INFO_CUSTOM(getset); /* A variant of PG_FUNCTION_INFO_V1 */

/*
 * Entry point from Postgres
 */
PGDLLEXPORT Datum getset(PG_FUNCTION_ARGS) {
  /* Initialize on first call */
  if (SRF_IS_FIRSTCALL()) getset_init(fcinfo);

  FuncCallContext *funcctx = SRF_PERCALL_SETUP();

  /* Get user context values that are kept across calls */
  UIFC *puifc = (UIFC *)funcctx->user_fctx;
  K colnames = kK(puifc->table->k)[0]; /* Column names of kdb+ result */
  K values = kK(puifc->table->k)[1];   /* Columns of the kdb+ result */
  int *perm = puifc->perm; /* Permutation order that map table columns with
                              result columns */
  int *todtind = puifc->todtind; /* Indices into typo-oid dispatch table */

  AttInMetadata *attinmeta = funcctx->attinmeta;
  int natts =
      attinmeta->tupdesc->natts; /* Number of attributes (result columns) */

  /* Place a kdb+ table row into a tuple */
  if (funcctx->call_cntr < kK(values)[0]->n) {
    /* Initialize components that make up the tuple (data and null indicators)
     */
    Datum *dvalues = puifc->dvalues;
    bool *nulls = puifc->nulls;

    /* Convert columns from kdb+ format to Postgres format */
    for (int i = 0; i < natts; i++) {
      PGEntry entry = (todt[todtind[i]].k2p)(
          kK(values)[perm[i]],    /* kdb+ column array */
          funcctx->call_cntr,     /* Current row to fetch */
          kS(colnames)[perm[i]]); /* kdb+ column name (for error reporting) */
      dvalues[i] = entry.dval;
      nulls[i] = entry.isNull;
    }

    /* Create a tuple given a complete row of values */
    HeapTuple tuple = heap_form_tuple(attinmeta->tupdesc, dvalues, nulls);

    /* Free up space used by those Datum types that are references */
    for (int i = 0; i < natts; i++)
      if (todt[todtind[i]].isref) pfree((void *)dvalues[i]);

    Datum result = HeapTupleGetDatum(tuple); /* Convert tuple to Datum */

    SRF_RETURN_NEXT(funcctx, result);
  } else /* no more rows to return */
  {
    r0(puifc->table); /* Free up memory used up by kdb+: r0(result) */
    SRF_RETURN_DONE(funcctx);
  }
}

/*
 * First call initialization (validation, connection, fetch kdb + table
 */
void getset_init(FunctionCallInfo fcinfo) {
  /* Create a function context for cross-call persistence */
  FuncCallContext *funcctx = SRF_FIRSTCALL_INIT();

  /* Switch to memory context appropriate for multiple function calls */
  MemoryContext oldcontext =
      MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

  /* Build a tuple descriptor for our result type */
  TupleDesc tupdesc;
  TypeFuncClass tfc = get_call_result_type(fcinfo, NULL, &tupdesc);
  if (tfc != TYPEFUNC_COMPOSITE)
    elog(ERROR, "Function must use composite types");

  /* Connect to a kdb+ process */
  I handle = khpu(host, port, userpass);
  if (handle <= 0)
    elog(ERROR, "Socket connection error (%d) attempting to connect to kdb+",
         handle);

  /* Get Postgres function arguments as a kdb+ array */
  K args = getset_args(fcinfo);

  /* Convert first argument (q expression or function) to a cstring */
  S ef = text_to_cstring(PG_GETARG_VARCHAR_PP(0));

  /* Call kdb+ and retrieve table */
  K table = kk(handle, ef, args);
  kclose(handle);

  /* Ensure result is a simple (unkeyed) table */
  if (!table)
    elog(ERROR, "Network error communicating with kdb+");
  else if (-128 == table->t) {
    char *p = pstrdup(TX(S, table)); /* need to duplicate error string */
    r0(table);                       /* before table gets deallocated */
    elog(ERROR, "kdb: %s", p);
  } else if (XT != table->t) {
    r0(table);
    elog(ERROR, "Result from kdb+ must be unkeyed table");
  }

  /* Generate attribute metadata needed later to produce tuples */
  AttInMetadata *attinmeta = TupleDescGetAttInMetadata(tupdesc);
  funcctx->attinmeta = attinmeta;

  /* Number of attributes (columns) in result */
  int natts = attinmeta->tupdesc->natts;
  int *perm = (int *)palloc(natts * sizeof(int));
  int *todtind = (int *)palloc(natts * sizeof(int));
  K colnames = kK(table->k)[0]; /* kdb+ column names */

  /* Loop through each result attribute (column) */
  for (int i = 0; i < natts; i++) {
    /* Find matching kdb+ column */
    char *attname = attinmeta->tupdesc->attrs[i]->attname.data;
    int pos = findName(attname, colnames);
    if (pos == -1)
      elog(ERROR, "Unable to match column name \"%s\" in kdb+ table", attname);
    perm[i] = pos;

    /* Find matching data type conversion */
    pos = findOID(attinmeta->tupdesc->attrs[i]->atttypid);
    if (pos == -1 || todt[pos].k2p == NULL)
      elog(ERROR, "Extension does not support datatype in column \"%s\"",
           attname);
    todtind[i] = pos;
  }

  /* Keep values between calls in user context */
  UIFC *puifc = (UIFC *)palloc0(sizeof(UIFC));
  puifc->table =
      table; /* Keep kdb+ result in memory until all rows are returned */
  puifc->perm = perm;
  puifc->todtind = todtind;
  puifc->dvalues =
      (Datum *)palloc(natts * sizeof(Datum)); /* Datum for 1 row, all columns */
  puifc->nulls =
      (bool *)palloc0(natts * sizeof(bool)); /* We don't support nulls */

  funcctx->user_fctx = puifc;

  MemoryContextSwitchTo(oldcontext);
}

/*
 * Get calling Postgres function's arguments
 */
K getset_args(FunctionCallInfo fcinfo) {
  /* Get the OID type list of the arguments for this function. */
  Oid funcid = fcinfo->flinfo->fn_oid; /* Function's OID */
  /* Get function definition tuple from Postgres */
  HeapTuple tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
  if (!HeapTupleIsValid(tp))
    elog(ERROR, "Cache lookup failed for function %u", funcid);
  Form_pg_proc procform = (Form_pg_proc)GETSTRUCT(tp); /* Copy tuple values */
  ReleaseSysCache(tp);
  int *pargoids = (int *)procform->proargtypes.values; /* Shorthand */

  //! Could I use this instead? get_fn_expr_argtype(). Not as fast as above.

  /* There has to be at least one argument and it must be varchar */
  int nargs = PG_NARGS();
  if (nargs == 0) elog(ERROR, "Function must have at least one argument");

  if (pargoids[0] != VARCHAROID)
    elog(ERROR,
         "Function first argument must be a varchar (kdb+ expression or "
         "function)");

  /* Initialize mixed K array (to be populated below) */
  K lo = knk(nargs - 1, 0);

  for (int i = 1; i < nargs; i++) {
    int oid = pargoids[i]; /* Argument type */

    int ind = findOID(oid); /* Get data conversion */
    if (ind < 0 || todt[ind].p2k == NULL)
      elog(ERROR, "Argument %d uses an unsupported type", i + 1);

    /* Call conversion routine via dispatch table */
    kK(lo)[i - 1] = (todt[ind].p2k)(PG_GETARG_DATUM(i));
  }

  return lo;
}

/*
 * Placeholder function until Kx implements this in their next C API release
 */
K kk(I h, char *f, K lo) {
  J n = 0;
  K *p = NULL;

  if (lo != NULL) {
    p = kK(lo);
    n = lo->n;
  }

  switch (n) {
    case 0:
      return k(h, f, (K)0);
    case 1:
      return k(h, f, p[0], (K)0);
    case 2:
      return k(h, f, p[0], p[1], (K)0);
    case 3:
      return k(h, f, p[0], p[1], p[2], (K)0);
    case 4:
      return k(h, f, p[0], p[1], p[2], p[3], (K)0);
    case 5:
      return k(h, f, p[0], p[1], p[2], p[3], p[4], (K)0);
    case 6:
      return k(h, f, p[0], p[1], p[2], p[3], p[4], p[5], (K)0);
    case 7:
      return k(h, f, p[0], p[1], p[2], p[3], p[4], p[5], p[6], (K)0);
    case 8:
      return k(h, f, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], (K)0);
    default:
      elog(ERROR, "The number of kdb+ function parameters exceeds 8");
      return 0;
  }
}
