/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file parses the real action of the VCL code, the procedure
 * statements which do the actual work.
 */

#include "config.h"

#include <string.h>

#include "vcc_compile.h"

/*--------------------------------------------------------------------*/

static void
parse_call(struct vcc *tl)
{

	vcc_NextToken(tl);
	ExpectErr(tl, ID);
	vcc_AddCall(tl, tl->t);
	vcc_AddRef(tl, tl->t, SYM_SUB);
	Fb(tl, 1, "if (VGC_function_%.*s(ctx))\n", PF(tl->t));
	Fb(tl, 1, "\treturn (1);\n");
	vcc_NextToken(tl);
}

/*--------------------------------------------------------------------*/

static const struct arith {
	vcc_type_t		type;
	unsigned		oper;
	vcc_type_t		want;
} arith[] = {
	{ INT,		T_INCR,		INT },
	{ INT,		T_DECR,		INT },
	{ INT,		T_MUL,		INT },
	{ INT,		T_DIV,		INT },
	{ INT,		'=',		INT },
	{ INT,		0,		INT },
	{ TIME,		T_INCR,		DURATION },
	{ TIME,		T_DECR,		DURATION },
	{ TIME,		T_MUL,		REAL },
	{ TIME,		T_DIV,		REAL },
	{ TIME,		'=',		TIME },
	{ TIME,		0,		TIME },
	{ DURATION,	T_INCR,		DURATION },
	{ DURATION,	T_DECR,		DURATION },
	{ DURATION,	T_MUL,		REAL },
	{ DURATION,	T_DIV,		REAL },
	{ DURATION,	'=',		DURATION },
	{ DURATION,	0,		DURATION },
	{ VOID,		'=',		VOID }
};

static void
parse_set(struct vcc *tl)
{
	const struct symbol *sym;
	const struct arith *ap;
	vcc_type_t fmt;

	vcc_NextToken(tl);
	ExpectErr(tl, ID);
	sym = vcc_FindVar(tl, tl->t, 1, "cannot be set");
	ERRCHK(tl);
	assert(sym != NULL);
	Fb(tl, 1, "%s\n", sym->lname);
	tl->indent += INDENT;
	vcc_NextToken(tl);
	fmt = sym->fmt;
	for (ap = arith; ap->type != VOID; ap++) {
		if (ap->type != fmt)
			continue;
		if (ap->oper != tl->t->tok)
			continue;
		if (ap->oper != '=')
			Fb(tl, 1, "%s %c ", sym->rname, *tl->t->b);
		vcc_NextToken(tl);
		fmt = ap->want;
		break;
	}
	if (ap->type == VOID)
		SkipToken(tl, ap->oper);
	if (fmt == HEADER) {
		vcc_Expr(tl, STRING_LIST);
	} else if (fmt == STRING) {
		vcc_Expr(tl, STRING_LIST);
	} else {
		vcc_Expr(tl, fmt);
	}
	tl->indent -= INDENT;
	Fb(tl, 1, ");\n");
}

/*--------------------------------------------------------------------*/

static void
parse_unset(struct vcc *tl)
{
	const struct symbol *sym;

	/* XXX: Wrong, should use VCC_Expr(HEADER) */
	vcc_NextToken(tl);
	ExpectErr(tl, ID);
	sym = vcc_FindVar(tl, tl->t, 1, "cannot be unset");
	ERRCHK(tl);
	assert(sym != NULL);
	if (sym->fmt != HEADER) {
		VSB_printf(tl->sb,
		    "Only HTTP header variables can be unset.\n");
		vcc_ErrWhere(tl, tl->t);
		return;
	}
	ERRCHK(tl);
	Fb(tl, 1, "%svrt_magic_string_unset);\n", sym->lname);
	vcc_NextToken(tl);
}

/*--------------------------------------------------------------------*/

static void
parse_new(struct vcc *tl)
{
	struct symbol *sy1, *sy2, *sy3;
	struct inifin *ifp;
	const char *p, *s_obj, *s_init, *s_struct, *s_fini;
	char buf1[128];
	char buf2[128];

	vcc_NextToken(tl);
	ExpectErr(tl, ID);
	vcc_ExpectCid(tl, "VCL object");
	ERRCHK(tl);
	sy1 = VCC_HandleSymbol(tl, tl->t, INSTANCE, "XXX");
	ERRCHK(tl);

	/* We allow implicit use of VMOD objects:  Pretend it's ref'ed */
	sy1->nref++;

	vcc_NextToken(tl);

	ExpectErr(tl, '=');
	vcc_NextToken(tl);

	ExpectErr(tl, ID);
	sy2 = VCC_SymbolTok(tl, NULL, tl->t, SYM_OBJECT, 0);
	if (sy2 == NULL) {
		VSB_printf(tl->sb, "Symbol not found: ");
		vcc_ErrToken(tl, tl->t);
		VSB_printf(tl->sb, " at ");
		vcc_ErrWhere(tl, tl->t);
		return;
	}
	XXXAN(sy2);

	/*lint -save -e448 */
	/* Split the first three args */
	p = sy2->args;

	s_obj = p;
	p += strlen(p) + 1;

	s_struct = p;
	p += strlen(p) + 1;

	s_init = p;
	while (p[0] != '\0' || p[1] != '\0' || p[2] != '\0')
		p++;
	p += 3;

	s_fini = p;
	while (p[0] != '\0' || p[1] != '\0' || p[2] != '\0')
		p++;
	p += 3;

	Fh(tl, 0, "static %s *vo_%s;\n\n", s_struct, sy1->name);

	vcc_NextToken(tl);

	bprintf(buf1, ", &vo_%s, \"%s\"", sy1->name, sy1->name);
	vcc_Eval_Func(tl, s_init, buf1, sy2->name, s_init + strlen(s_init) + 1,
	    sy2->vmod);
	ifp = New_IniFin(tl);
	VSB_printf(ifp->fin, "\t\t%s(&vo_%s);", s_fini, sy1->name);
	ExpectErr(tl, ';');

	bprintf(buf1, ", vo_%s", sy1->name);
	/* Split the methods from the args */
	while (*p != '\0') {
		p += strlen(s_obj);
		bprintf(buf2, "%s%s", sy1->name, p);
		sy3 = VCC_Symbol(tl, NULL, buf2, NULL, SYM_FUNC, 1);
		AN(sy3);
		sy3->eval = vcc_Eval_SymFunc;
		p += strlen(p) + 1;
		sy3->cfunc = p;
		p += strlen(p) + 1;

		sy3->args = p;
		sy3->extra = TlDup(tl, buf1);
		while (p[0] != '\0' || p[1] != '\0' || p[2] != '\0')
			p++;
		p += 3;
	}
	sy1->def_e = tl->t;
	/*lint -restore */
}

/*--------------------------------------------------------------------*/

static void
parse_ban(struct vcc *tl)
{

	vcc_NextToken(tl);

	ExpectErr(tl, '(');
	vcc_NextToken(tl);

	Fb(tl, 1, "VRT_ban_string(ctx, \n");
	tl->indent += INDENT;
	vcc_Expr(tl, STRING);
	tl->indent -= INDENT;
	ERRCHK(tl);
	Fb(tl, 1, ");\n");

	ExpectErr(tl, ')');
	vcc_NextToken(tl);
}

/*--------------------------------------------------------------------*/

static void
parse_hash_data(struct vcc *tl)
{
	vcc_NextToken(tl);
	SkipToken(tl, '(');

	Fb(tl, 1, "VRT_hashdata(ctx,\n  ");
	vcc_Expr(tl, STRING_LIST);
	ERRCHK(tl);
	Fb(tl, 1, ");\n");
	SkipToken(tl, ')');
}

/*--------------------------------------------------------------------*/

static void
parse_return(struct vcc *tl)
{
	int retval = 0;

	vcc_NextToken(tl);
	ExpectErr(tl, '(');
	vcc_NextToken(tl);
	ExpectErr(tl, ID);

	/* 'error' gets special handling, to allow optional status/response */
	if (vcc_IdIs(tl->t, "synth")) {
		vcc_NextToken(tl);
		if (tl->t->tok == ')') {
			VSB_printf(tl->sb,
			    "Syntax has changed, use:\n"
			    "\treturn(synth(999));\n"
			    "or\n"
			    "\treturn(synth(999, \"Response text\"));\n");
			vcc_ErrWhere(tl, tl->t);
			return;
		}
		ExpectErr(tl, '(');
		vcc_NextToken(tl);
		Fb(tl, 1, "VRT_synth(ctx,\n");
		tl->indent += INDENT;
		vcc_Expr(tl, INT);
		ERRCHK(tl);
		Fb(tl, 1, ",\n");
		if (tl->t->tok == ',') {
			vcc_NextToken(tl);
			vcc_Expr(tl, STRING);
			ERRCHK(tl);
		} else {
			Fb(tl, 1, "(const char*)0\n");
		}
		tl->indent -= INDENT;
		ExpectErr(tl, ')');
		vcc_NextToken(tl);
		Fb(tl, 1, ");\n");
		Fb(tl, 1, "VRT_handling(ctx, VCL_RET_SYNTH);\n");
		Fb(tl, 1, "return (1);\n");
		vcc_ProcAction(tl->curproc, VCL_RET_SYNTH, tl->t);
		ExpectErr(tl, ')');
		vcc_NextToken(tl);
		return;
	}

#define VCL_RET_MAC(l, U, B)						\
	do {								\
		if (vcc_IdIs(tl->t, #l)) {				\
			Fb(tl, 1, "VRT_handling(ctx, VCL_RET_" #U ");\n"); \
			Fb(tl, 1, "return (1);\n");			\
			vcc_ProcAction(tl->curproc, VCL_RET_##U, tl->t);\
			retval = 1;					\
		}							\
	} while (0);
#include "tbl/vcl_returns.h"
#undef VCL_RET_MAC
	if (!retval) {
		VSB_printf(tl->sb, "Expected return action name.\n");
		vcc_ErrWhere(tl, tl->t);
		ERRCHK(tl);
	}
	vcc_NextToken(tl);
	ExpectErr(tl, ')');
	vcc_NextToken(tl);
}

/*--------------------------------------------------------------------*/

static void
parse_synthetic(struct vcc *tl)
{
	vcc_NextToken(tl);

	ExpectErr(tl, '(');
	ERRCHK(tl);
	vcc_NextToken(tl);

	Fb(tl, 1, "VRT_synth_page(ctx, ");
	vcc_Expr(tl, STRING_LIST);
	ERRCHK(tl);
	Fb(tl, 0, ");\n");

	ExpectErr(tl, ')');
	vcc_NextToken(tl);
	ERRCHK(tl);
}

/*--------------------------------------------------------------------*/

typedef void action_f(struct vcc *tl);

static struct action_table {
	const char		*name;
	action_f		*func;
	unsigned		bitmask;
} action_table[] = {
	/* Keep list sorted from here */
	{ "ban",		parse_ban },
	{ "call",		parse_call },
	{ "hash_data",		parse_hash_data, VCL_MET_HASH },
	{ "new",		parse_new, VCL_MET_INIT},
	{ "return",		parse_return },
	{ "set",		parse_set },
	{ "synthetic",		parse_synthetic,
		VCL_MET_SYNTH | VCL_MET_BACKEND_ERROR },
	{ "unset",		parse_unset },
	{ NULL,			NULL }
};

int
vcc_ParseAction(struct vcc *tl)
{
	struct token *at;
	struct action_table *atp;
	const struct symbol *sym;

	at = tl->t;
	assert(at->tok == ID);
	for(atp = action_table; atp->name != NULL; atp++) {
		if (vcc_IdIs(at, atp->name)) {
			if (atp->bitmask != 0)
				vcc_AddUses(tl, at, atp->bitmask,
				    "not a valid action");
			atp->func(tl);
			return (1);
		}
	}
	sym = VCC_SymbolTok(tl, NULL, tl->t, SYM_NONE, 0);
	if (sym != NULL && sym->kind == SYM_FUNC) {
		vcc_Expr_Call(tl, sym);
		return (1);
	}
	return (0);
}
