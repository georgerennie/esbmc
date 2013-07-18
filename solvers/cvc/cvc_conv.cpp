#include <ansi-c/c_types.h>

#include "cvc_conv.h"

prop_convt *
create_new_cvc_solver(bool int_encoding, bool is_cpp, const namespacet &ns)
{
    return new cvc_convt(is_cpp, int_encoding, ns);
}

cvc_convt::cvc_convt(bool is_cpp, bool int_encoding, const namespacet &ns)
   : smt_convt(true, int_encoding, ns, is_cpp, false, true, false),
     em(), smt(&em)
{
  // Already initialized stuff in the constructor list,

  smt.setOption("produce-models", true);
  smt.setLogic("QF_AUFBV");

  assert(!int_encoding && "Integer encoding mode for CVC unimplemented");

  smt_post_init();
}

cvc_convt::~cvc_convt()
{
}

prop_convt::resultt
cvc_convt::dec_solve()
{
  CVC4::Result r = smt.checkSat();
  if (r.isSat())
    return P_SATISFIABLE;
  else if (!r.isUnknown())
    return P_UNSATISFIABLE;
  else {
    std::cerr << "Error solving satisfiability of formula with CVC"
              << std::endl;
    abort();
  }
}

expr2tc
cvc_convt::get(const expr2tc &expr)
{
  switch (expr->type->type_id) {
  case type2t::bool_id:
    return get_bool(convert_ast(expr));
  case type2t::unsignedbv_id:
  case type2t::signedbv_id:
    return get_bv(convert_ast(expr));
  case type2t::fixedbv_id:
  {
    // XXX -- again, another candidate for refactoring.
    expr2tc tmp = get_bv(convert_ast(expr));
    const constant_int2t &intval = to_constant_int2t(tmp);
    uint64_t val = intval.constant_value.to_ulong();
    std::stringstream ss;
    ss << val;
    constant_exprt value_expr(migrate_type_back(expr->type));
    value_expr.set_value(get_fixed_point(expr->type->get_width(), ss.str()));
    fixedbvt fbv;
    fbv.from_expr(value_expr);
    return constant_fixedbv2tc(expr->type, fbv);
  }
  case type2t::array_id:
    return get_array(convert_ast(expr), expr->type);
  case type2t::struct_id:
  case type2t::union_id:
  case type2t::pointer_id:
    return tuple_get(expr);
  default:
    std::cerr << "Unimplemented type'd expression (" << expr->type->type_id
              << ") in cvc get" << std::endl;
    abort();
  }
}

tvt
cvc_convt::l_get(literalt l)
{
  const cvc_smt_ast *ca = cvc_ast_downcast(lit_to_ast(l));
  constant_bool2tc b = get_bool(ca);
  if (b->constant_value)
    return tvt(true);
  else
    return tvt(false);
}

expr2tc
cvc_convt::get_bool(const smt_ast *a)
{
  const cvc_smt_ast *ca = cvc_ast_downcast(a);
  CVC4::Expr e = smt.getValue(ca->e);
  bool foo = e.getConst<bool>();
  return constant_bool2tc(foo);
}

expr2tc
cvc_convt::get_bv(const smt_ast *a)
{
  const cvc_smt_ast *ca = cvc_ast_downcast(a);
  CVC4::Expr e = smt.getValue(ca->e);
  CVC4::BitVector foo = e.getConst<CVC4::BitVector>();
  // huuurrrrrrrr, an immense lack of uint64_t's detected.
  uint64_t val = foo.toInteger().getUnsignedLong();
  return constant_int2tc(get_uint_type(foo.getSize()), BigInt(val));
}

expr2tc
cvc_convt::get_array(const smt_ast *a, const type2tc &t)
{

  const array_type2t &ar = to_array_type(t);
  if (is_tuple_ast_type(ar.subtype)) {
    std::cerr << "Tuple array getting not implemented yet, sorry" << std::endl;
    return expr2tc();
  }

  // Fetch the array bounds, if it's huge then assume this is a 1024 element
  // array. Then fetch all elements and formulate a constant_array.
  size_t w = a->sort->get_domain_width();
  size_t orig_w = w;
  if (w > 10)
    w = 10;

  const cvc_smt_ast *ca = cvc_ast_downcast(a);
  constant_int2tc arr_size(index_type2(), BigInt(1 << w));

  type2tc arr_type = type2tc(new array_type2t(ar.subtype, arr_size, false));
  const smt_sort *s = convert_sort(ar.subtype);
  std::vector<expr2tc> fields;

  for (size_t i = 0; i < (1ULL << w); i++) {
    smt_ast *tmpast = mk_smt_bvint(BigInt(i), false, orig_w);
    const cvc_smt_ast *tmpa = cvc_ast_downcast(tmpast);
    CVC4::Expr e = em.mkExpr(CVC4::kind::SELECT, ca->e, tmpa->e);
    free(tmpast);

    cvc_smt_ast *tmpb = new cvc_smt_ast(s, e);
    fields.push_back(get_bv(tmpb));
    free(tmpb);
  }

  return constant_array2tc(arr_type, fields);
}

const std::string
cvc_convt::solver_text()
{
  std::stringstream ss;
  ss << "CVC " << CVC4::Configuration::getVersionString();
  return ss.str();
}

void
cvc_convt::assert_lit(const literalt &l)
{
  const cvc_smt_ast *ca = cvc_ast_downcast(lit_to_ast(l));
  smt.assertFormula(ca->e);
}

smt_ast *
cvc_convt::mk_func_app(const smt_sort *s, smt_func_kind k,
                             const smt_ast * const *_args,
                             unsigned int numargs)
{
  const cvc_smt_ast *args[4];
  unsigned int i;

  assert(numargs <= 4);
  for (i = 0; i < numargs; i++)
    args[i] = cvc_ast_downcast(_args[i]);

  CVC4::Expr e;

  switch (k) {
  case SMT_FUNC_EQ:
    if (args[0]->sort->id == SMT_SORT_BOOL) {
      e = em.mkExpr(CVC4::kind::IFF, args[0]->e, args[1]->e);
    } else {
      e = em.mkExpr(CVC4::kind::EQUAL, args[0]->e, args[1]->e);
    }
    break;
  case SMT_FUNC_NOTEQ:
    e = em.mkExpr(CVC4::kind::DISTINCT, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_AND:
    e = em.mkExpr(CVC4::kind::AND, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_OR:
    e = em.mkExpr(CVC4::kind::OR, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_IMPLIES:
    e = em.mkExpr(CVC4::kind::IMPLIES, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_ITE:
    e = em.mkExpr(CVC4::kind::ITE, args[0]->e, args[1]->e, args[2]->e);
    break;
  case SMT_FUNC_NOT:
    e = em.mkExpr(CVC4::kind::NOT, args[0]->e);
    break;
  case SMT_FUNC_BVADD:
    e = em.mkExpr(CVC4::kind::BITVECTOR_PLUS, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVSUB:
    e = em.mkExpr(CVC4::kind::BITVECTOR_SUB, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVMUL:
    e = em.mkExpr(CVC4::kind::BITVECTOR_MULT, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVLSHR:
    e = em.mkExpr(CVC4::kind::BITVECTOR_LSHR, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVASHR:
    e = em.mkExpr(CVC4::kind::BITVECTOR_ASHR, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVSHL:
    e = em.mkExpr(CVC4::kind::BITVECTOR_SHL, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVUGT:
    e = em.mkExpr(CVC4::kind::BITVECTOR_UGT, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVUGTE:
    e = em.mkExpr(CVC4::kind::BITVECTOR_UGE, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVULT:
    e = em.mkExpr(CVC4::kind::BITVECTOR_ULT, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVULTE:
    e = em.mkExpr(CVC4::kind::BITVECTOR_ULE, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVSGT:
    e = em.mkExpr(CVC4::kind::BITVECTOR_SGT, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVSGTE:
    e = em.mkExpr(CVC4::kind::BITVECTOR_SGE, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVSLT:
    e = em.mkExpr(CVC4::kind::BITVECTOR_SLT, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_BVSLTE:
    e = em.mkExpr(CVC4::kind::BITVECTOR_SLE, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_STORE:
    e = em.mkExpr(CVC4::kind::STORE, args[0]->e, args[1]->e, args[2]->e);
    break;
  case SMT_FUNC_SELECT:
    e = em.mkExpr(CVC4::kind::SELECT, args[0]->e, args[1]->e);
    break;
  case SMT_FUNC_CONCAT:
    e = em.mkExpr(CVC4::kind::BITVECTOR_CONCAT, args[0]->e, args[1]->e);
    break;
  default:
    std::cerr << "Unimplemented SMT function \"" << smt_func_name_table[k]
              << "\" in CVC conversion" << std::endl;
    abort();
  }

  return new cvc_smt_ast(s, e);
}

smt_sort *
cvc_convt::mk_sort(const smt_sort_kind k, ...)
{
  va_list ap;
  unsigned long uint;
  int thebool;

  va_start(ap, k);
  switch (k) {
  case SMT_SORT_BOOL:
  {
    CVC4::BooleanType t = em.booleanType();
    return new cvc_smt_sort(k, t);
  }
  case SMT_SORT_BV:
  {
    uint = va_arg(ap, unsigned long);
    thebool = va_arg(ap, int);
    thebool = thebool;
    CVC4::BitVectorType t = em.mkBitVectorType(uint);
    return new cvc_smt_sort(k, t);
  }
  case SMT_SORT_ARRAY:
  {
    const cvc_smt_sort *dom = va_arg(ap, const cvc_smt_sort*);
    const cvc_smt_sort *range = va_arg(ap, const cvc_smt_sort*);
    CVC4::ArrayType t = em.mkArrayType(dom->t, range->t);
    cvc_smt_sort *cs = new cvc_smt_sort(k, t);
    CVC4::BitVectorType bv_type(dom->t);
    cs->array_dom_width = bv_type.getSize();
    return cs;
  }
  default:
    std::cerr << "Unimplemented smt sort " << k << " in CVC mk_sort"
              << std::endl;
    abort();
  }
}

literalt
cvc_convt::mk_lit(const smt_ast *a)
{
  const cvc_smt_ast *cast = cvc_ast_downcast(a);
  literalt l = new_variable();
  const cvc_smt_ast *c2 = cvc_ast_downcast(lit_to_ast(l));

  CVC4::Expr e = em.mkExpr(CVC4::Kind::IFF, cast->e, c2->e);
  smt.assertFormula(e);
  return l;
}

smt_ast *
cvc_convt::mk_smt_int(const mp_integer &theint __attribute__((unused)), bool sign __attribute__((unused)))
{
  abort();
}

smt_ast *
cvc_convt::mk_smt_real(const std::string &str __attribute__((unused)))
{
  abort();
}

smt_ast *
cvc_convt::mk_smt_bvint(const mp_integer &theint, bool sign, unsigned int w)
{
  const smt_sort *s = mk_sort(SMT_SORT_BV, w, false);

  // Seems we can't make negative bitvectors; so just pull the value out and
  // assume CVC is going to cut the top off correctly.
  CVC4::BitVector bv = CVC4::BitVector(w, (uint64_t)theint.to_ulong());
  CVC4::Expr e = em.mkConst(bv);
  return new cvc_smt_ast(s, e);
}

smt_ast *
cvc_convt::mk_smt_bool(bool val)
{
  const smt_sort *s = mk_sort(SMT_SORT_BOOL);
  CVC4::Expr e = em.mkConst(val);
  return new cvc_smt_ast(s, e);
}

smt_ast *
cvc_convt::mk_smt_symbol(const std::string &name, const smt_sort *s)
{
  const cvc_smt_sort *sort = cvc_sort_downcast(s);
  CVC4::Expr e = em.mkVar(name, sort->t); // "global", eh?
  return new cvc_smt_ast(s, e);
}

smt_sort *
cvc_convt::mk_struct_sort(const type2tc &type __attribute__((unused)))
{
  abort();
}

smt_sort *
cvc_convt::mk_union_sort(const type2tc &type __attribute__((unused)))
{
  abort();
}

smt_ast *
cvc_convt::mk_extract(const smt_ast *a, unsigned int high,
                            unsigned int low, const smt_sort *s)
{
  const cvc_smt_ast *ca = cvc_ast_downcast(a);
  CVC4::BitVectorExtract ext(high, low);
  CVC4::Expr ext2 = em.mkConst(ext);
  CVC4::Expr fin = em.mkExpr(CVC4::Kind::BITVECTOR_EXTRACT, ext2, ca->e);
  return new cvc_smt_ast(s, fin);
}
