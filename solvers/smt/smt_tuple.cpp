#include <sstream>

#include <ansi-c/c_types.h>

#include <base_type.h>

#include "smt_conv.h"

/** @file smt_tuple.cpp
 * So, the SMT-encoding-with-no-tuple-support. SMT itself doesn't support
 * tuples, but we've been pretending that it does by using Z3 almost
 * exclusively (which does support tuples). So, we have to find some way of
 * supporting:
 *
 *  1) Tuples
 *  2) Arrays of tuples
 *  3) Arrays of tuples containing arrays
 *
 * 1: In all circumstances we're either creating, projecting, or updating,
 *    a set of variables that are logically grouped together as a tuple. We
 *    can generally just handle that by linking up a tuple to the actual
 *    variable that we want. To do this, we create a set of symbols /underneath/
 *    the symbol we're dealing with, corresponding to the field name. So a tuple
 *    with fields a, b, and c, with the symbol name "faces" would create:
 *
 *      c::main::1::faces.a
 *      c::main::1::faces.b
 *      c::main::1::faces.c
 *
 *    As variables with the appropriate type. Project / update redirects
 *    expressions to deal with those symbols. Equality is similar.
 *
 *    It gets more complicated with ite's though, as you can't
 *    nondeterministically switch between symbol prefixes like that. So instead
 *    we create a fresh new symbol, and create an ite for each member of the
 *    tuple, binding it into the new fresh symbol if it's enabling condition
 *    is true.
 *
 *    The basic design feature here is that in all cases where we have something
 *    of tuple type, the AST is actually a deterministic symbol that we hang
 *    additional bits of name off as appropriate.
 *
 *  2: For arrays of tuples, we do almost exactly as above, but all the new
 *     variables that are used are in fact arrays of values. Then when we
 *     perform an array operation, we either select the relevant values out into
 *     a fresh tuple, or decompose a tuple into a series of updates to make.
 *
 *  3: Tuples of arrays of tuples are currently unimplemented. But it's likely
 *     that we'll end up following 2: above, but extending the domain of the
 *     array to contain the outer and inner array index. Ugly but works
 *     (inspiration came from the internet / stackoverflow, reading other
 *     peoples work where they've done this).
 *
 * All these things could probably be made more efficient by rewriting the
 * expressions that are being converted, so that we can for example discard
 * any un-necessary equalities or assertions. However, in the meantime, this
 * slower approach works.
 */

void
tuple_smt_ast::make_free(smt_convt *ctx)
{
  if (elements.size() != 0)
    return;

  tuple_smt_sortt ts = to_tuple_sort(sort);
  const struct_union_data &strct = ctx->get_type_def(ts->thetype);

  elements.resize(strct.members.size());
  unsigned int i = 0;
  forall_types(it, strct.members) {
    smt_sortt newsort = ctx->convert_sort(*it);
    std::string fieldname = name + "." + strct.member_names[i].as_string();

    if (is_tuple_ast_type(*it)) {
      elements[i] = ctx->tuple_fresh(newsort, fieldname);
    } else if (is_tuple_array_ast_type(*it)) {
      elements[i] = new array_smt_ast(ctx, newsort, fieldname);
    } else {
      elements[i] = ctx->mk_fresh(newsort, fieldname);
    }

    i++;
  }
}

smt_astt 
smt_ast::ite(smt_convt *ctx, smt_astt cond, smt_astt falseop) const
{
  return ctx->mk_func_app(sort, SMT_FUNC_ITE, cond, this, falseop);
}

smt_astt 
tuple_smt_ast::ite(smt_convt *ctx, smt_astt cond, smt_astt falseop) const
{
  // So - we need to generate an ite between true_val and false_val, that gets
  // switched on based on cond, and store the output into result. Do this by
  // projecting each member out of our arguments and computing another ite
  // over each member. Note that we always make assertions here, because the
  // ite is always true. We return the output symbol.
  tuple_smt_astt true_val = this;
  tuple_smt_astt false_val = to_tuple_ast(falseop);
  tuple_smt_sortt thissort = to_tuple_sort(sort);
  std::string name = ctx->mk_fresh_name("tuple_ite::") + ".";
  tuple_smt_ast *result_sym = new tuple_smt_ast(ctx, sort, name);

  const_cast<tuple_smt_ast*>(true_val)->make_free(ctx);
  const_cast<tuple_smt_ast*>(false_val)->make_free(ctx);

  const struct_union_data &data = ctx->get_type_def(thissort->thetype);
  result_sym->elements.resize(data.members.size());

  // Iterate through each field and encode an ite.
  unsigned int i = 0;
  forall_types(it, data.members) {
    smt_astt truepart = true_val->project(ctx, i);
    smt_astt falsepart = false_val->project(ctx, i);

    smt_astt result_ast = truepart->ite(ctx, cond, falsepart);

    result_sym->elements[i] = result_ast;

    i++;
  }

  return result_sym;
}

smt_astt 
array_smt_ast::ite(smt_convt *ctx, smt_astt cond, smt_astt falseop) const
{
  // Similar to tuple ite's, but the leafs are arrays.
  array_smt_astt false_val = to_array_ast(falseop);
  tuple_smt_sortt thissort = to_tuple_sort(sort);
  assert(is_array_type(thissort->thetype));
  const array_type2t &array_type = to_array_type(thissort->thetype);

  std::string name = ctx->mk_fresh_name("tuple_array_ite::") + ".";
  array_smt_ast *result_sym = new array_smt_ast(ctx, thissort, name);

  const struct_union_data &data = ctx->get_type_def(array_type.subtype);

  // Iterate through each field and encode an ite.
  unsigned int i = 0;
  forall_types(it, data.members) {
    type2tc arrtype(new array_type2t(*it, array_type.array_size,
          array_type.size_is_infinite));

    smt_astt truepart = elements[i];
    smt_astt falsepart = false_val->elements[i];

    smt_astt result_ast = truepart->ite(ctx, cond, falsepart);

    result_sym->elements[i] = result_ast;
    i++;
  }

  return result_sym;
}

smt_astt 
smt_ast::eq(smt_convt *ctx, smt_astt other) const
{
  // Simple approach: this is a leaf piece of SMT, compute a basic equality.
  smt_sortt boolsort = ctx->mk_sort(SMT_SORT_BOOL);
  return ctx->mk_func_app(boolsort, SMT_FUNC_EQ, this, other);
}

void
tuple_smt_ast::assign(smt_convt *ctx, tuple_smt_astt other) const
{
  const_cast<tuple_smt_ast*>(to_tuple_ast(other))->make_free(ctx);
  assert(elements.size() == 0 && "tuple smt assign with elems populated");

  tuple_smt_ast *destination = const_cast<tuple_smt_ast *>(this);

  // Just copy across element data.
  destination->elements = other->elements;
  return;
}

smt_astt 
tuple_smt_ast::eq(smt_convt *ctx, smt_astt other) const
{
  const_cast<tuple_smt_ast*>(to_tuple_ast(other))->make_free(ctx);

  // We have two tuple_smt_asts and need to create a boolean ast representing
  // their equality: iterate over all their members, compute an equality for
  // each of them, and then combine that into a final ast.
  tuple_smt_astt ta = this;
  tuple_smt_astt tb = to_tuple_ast(other);
  tuple_smt_sortt ts = to_tuple_sort(sort);
  const struct_union_data &data = ctx->get_type_def(ts->thetype);

  smt_convt::ast_vec eqs;
  eqs.reserve(data.members.size());

  // Iterate through each field and encode an equality.
  unsigned int i = 0;
  forall_types(it, data.members) {
    smt_astt side1 = ta->project(ctx, i);
    smt_astt side2 = tb->project(ctx, i);
    eqs.push_back(side1->eq(ctx, side2));
    i++;
  }

  // Create an ast representing the fact that all the members are equal.
  return ctx->make_conjunct(eqs);
}

void
array_smt_ast::assign(array_smt_astt src) const
{
  assert(is_still_free && "Non-free array ast assigned");
  array_smt_ast *destination = const_cast<array_smt_ast *>(this);

  // Just copy across element data.
  destination->elements = src->elements;
  destination->is_still_free = false;
}

smt_astt 
array_smt_ast::eq(smt_convt *ctx, smt_astt other) const
{

  // We have two tuple_smt_asts and need to create a boolean ast representing
  // their equality: iterate over all their members, compute an equality for
  // each of them, and then combine that into a final ast.
  array_smt_astt tb = to_array_ast(other);
  tuple_smt_sortt ts = to_tuple_sort(sort);
  assert(is_array_type(ts->thetype));
  const array_type2t &arrtype = to_array_type(ts->thetype);
  const struct_union_data &data = ctx->get_type_def(arrtype.subtype);

  smt_convt::ast_vec eqs;
  eqs.reserve(data.members.size());

  // Iterate through each field and encode an equality.
  unsigned int i = 0;
  forall_types(it, data.members) {
    type2tc tmparrtype(new array_type2t(*it, arrtype.array_size,
          arrtype.size_is_infinite));
    smt_astt side1 = elements[i];
    smt_astt side2 = tb->elements[i];
    eqs.push_back(side1->eq(ctx, side2));
    i++;
  }

  // Create an ast representing the fact that all the members are equal.
  return ctx->make_conjunct(eqs);
}

smt_astt 
smt_ast::update(smt_convt *ctx, smt_astt value, unsigned int idx,
    expr2tc idx_expr) const
{
  // If we're having an update applied to us, then the only valid situation
  // this can occur in is if we're an array.
  assert(sort->id == SMT_SORT_ARRAY);

  // We're an array; just generate a 'with' operation.
  expr2tc index;
  if (is_nil_expr(idx_expr)) {
    index = constant_int2tc(type2tc(new unsignedbv_type2t(sort->domain_width)),
          BigInt(idx));
  } else {
    index = idx_expr;
  }

  return ctx->mk_func_app(sort, SMT_FUNC_STORE,
                          this, ctx->convert_ast(index), value);
}

smt_astt 
tuple_smt_ast::update(smt_convt *ctx, smt_astt value, unsigned int idx,
    expr2tc idx_expr) const
{
  smt_convt::ast_vec eqs;
  assert(is_nil_expr(idx_expr) && "Can't apply non-constant index update to "
         "structure");

  std::string name = ctx->mk_fresh_name("tuple_update::") + ".";
  tuple_smt_ast *result = new tuple_smt_ast(ctx, sort, name);
  result->elements = elements;
  result->make_free(ctx);
  result->elements[idx] = value;

  return result;
}

smt_astt 
array_smt_ast::update(smt_convt *ctx, smt_astt value, unsigned int idx,
    expr2tc idx_expr) const
{

  tuple_smt_sortt ts = to_tuple_sort(sort);
  const array_type2t array_type = to_array_type(ts->thetype);
  const struct_union_data &data = ctx->get_type_def(array_type.subtype);

  expr2tc index;
  if (is_nil_expr(idx_expr)) {
    index = constant_int2tc(ctx->make_array_domain_sort_exp(array_type),
                            BigInt(idx));
  } else {
    index = idx_expr;
  }

  std::string name = ctx->mk_fresh_name("tuple_array_update::") + ".";
  array_smt_ast *result = new array_smt_ast(ctx, sort, name);

  // Iterate over all members. They are _all_ indexed and updated.
  unsigned int i = 0;
  forall_types(it, data.members) {
    type2tc arrtype(new array_type2t(*it, array_type.array_size,
          array_type.size_is_infinite));

    // Project and update a field in 'this'
    smt_astt field = elements[i];
    smt_astt resval = value->project(ctx, i);
    smt_astt updated = field->update(ctx, resval, 0, index);

    result->elements[i] = updated;

    i++;
  }

  return result;
}

smt_astt 
smt_ast::select(smt_convt *ctx, const expr2tc &idx) const
{
  assert(sort->id == SMT_SORT_ARRAY && "Select operation applied to non-array "
         "scalar AST");

  // Just apply a select operation to the current array. Index should be fixed.

  // Guess the resulting sort. This could be a lot, lot better.
  smt_sortt range_sort = NULL;
  if (sort->data_width == 1 && !ctx->no_bools_in_arrays)
    range_sort = ctx->mk_sort(SMT_SORT_BOOL);
  else
    range_sort = ctx->mk_sort(SMT_SORT_BV, sort->data_width, false); //XXX sign?

  return ctx->mk_func_app(range_sort, SMT_FUNC_SELECT,
                          this, ctx->convert_ast(idx));
}

smt_astt 
tuple_smt_ast::select(smt_convt *ctx __attribute__((unused)),
    const expr2tc &idx __attribute__((unused))) const
{
  std::cerr << "Select operation applied to tuple" << std::endl;
  abort();
}

smt_astt 
array_smt_ast::select(smt_convt *ctx, const expr2tc &idx) const
{
  tuple_smt_sortt ts = to_tuple_sort(sort);
  const array_type2t &array_type = to_array_type(ts->thetype);
  const struct_union_data &data = ctx->get_type_def(array_type.subtype);
  smt_sortt result_sort = ctx->convert_sort(array_type.subtype);

  std::string name = ctx->mk_fresh_name("tuple_array_select::") + ".";
  tuple_smt_ast *result = new tuple_smt_ast(ctx, result_sort, name);
  result->elements.resize(data.members.size());

  unsigned int i = 0;
  forall_types(it, data.members) {
    smt_astt sub_array = elements[i];
    smt_astt selected = sub_array->select(ctx, idx);
    result->elements[i] = selected;

    i++;
  }

  return result;
}

smt_astt 
smt_ast::project(smt_convt *ctx __attribute__((unused)),
    unsigned int idx __attribute__((unused))) const
{
  std::cerr << "Projecting from non-tuple based AST" << std::endl;
  abort();
}

smt_astt 
tuple_smt_ast::project(smt_convt *ctx, unsigned int idx) const
{
  // Create an AST representing the i'th field of the tuple a. This means we
  // have to open up the (tuple symbol) a, tack on the field name to the end
  // of that name, and then return that. It now names the variable that contains
  // the value of that field. If it's actually another tuple, we instead return
  // a new tuple_smt_ast containing its name.
  tuple_smt_sortt ts = to_tuple_sort(sort);
  const struct_union_data &data = ctx->get_type_def(ts->thetype);

  // If someone is projecting out of us, then now is an excellent time to
  // actually allocate all our pieces of ASTs as variables.
  const_cast<tuple_smt_ast*>(this)->make_free(ctx);

  assert(idx < data.members.size() && "Out-of-bounds tuple element accessed");
  return elements[idx];
}

smt_astt 
array_smt_ast::project(smt_convt *ctx __attribute__((unused)),
                       unsigned int idx) const
{

  assert(idx < elements.size() && "Out-of-bounds tuple-array element accessed");
  return elements[idx];
}

smt_astt
smt_convt::tuple_create(const expr2tc &structdef)
{
  // From a vector of expressions, create a tuple representation by creating
  // a fresh name and assigning members into it.
  std::string name = mk_fresh_name("tuple_create::");
  // Add a . suffix because this is of tuple type.
  name += ".";

  tuple_smt_ast *result =
    new tuple_smt_ast(this, convert_sort(structdef->type),name);
  result->elements.resize(structdef->get_num_sub_exprs());

  for (unsigned int i = 0; i < structdef->get_num_sub_exprs(); i++) {
    smt_astt tmp = convert_ast(*structdef->get_sub_expr(i));
    result->elements[i] = tmp;
  }

  return result;
}

smt_astt
smt_convt::union_create(const expr2tc &unidef)
{
  // Unions are known to be brok^W fragile. Create a free new structure, and
  // assign in any members where the type matches the single member of the
  // initializer members. No need to worry about subtypes; this is a union.
  std::string name = mk_fresh_name("union_create::");
  // Add a . suffix because this is of tuple type.
  name += ".";
  symbol2tc result(unidef->type, irep_idt(name));

  const constant_union2t &uni = to_constant_union2t(unidef);
  const struct_union_data &def = get_type_def(uni.type);
  assert(uni.datatype_members.size() == 1 && "Unexpectedly full union "
         "initializer");
  const expr2tc &init = uni.datatype_members[0];
  smt_astt result_ast = convert_ast(result);
  smt_astt init_ast = convert_ast(init);

  tuple_smt_ast *result_t_ast =
    const_cast<tuple_smt_ast *>(to_tuple_ast(result_ast));
  result_t_ast->elements.resize(def.members.size());

  unsigned int i = 0;
  forall_types(it, def.members) {
    if (base_type_eq(*it, init->type, ns)) {
      // Assign in.
      smt_astt target_memb = result_ast->project(this, i);
      assert_ast(target_memb->eq(this, init_ast));

      result_t_ast->elements[i] = init_ast;
    } else {
      // XXX indirection
      if (is_tuple_ast_type(*it)) {
        result_t_ast->elements[i] = tuple_fresh(convert_sort(*it));
      } else if (is_tuple_array_ast_type(*it)) {
        // XXX XXX XXX fresh array method?
        std::string name = mk_fresh_name("union_create_elem");
        smt_sortt sort = convert_sort(*it);
        result_t_ast->elements[i] = new array_smt_ast(this, sort, name);
      }
    }
    i++;
  }

  return new tuple_smt_ast(this, convert_sort(unidef->type), name);
}

smt_astt
smt_convt::tuple_fresh(smt_sortt s, std::string name)
{
  if (name == "")
    name = mk_fresh_name("tuple_fresh::") + ".";

  smt_astt a = mk_smt_symbol(name, s);
  (void)a;
  if (s->id == SMT_SORT_ARRAY)
    return new array_smt_ast(this, s, name);
  else
    return new tuple_smt_ast(this, s, name);
}

const struct_union_data &
smt_convt::get_type_def(const type2tc &type) const
{

  return (is_pointer_type(type))
        ? *pointer_type_data
        : dynamic_cast<const struct_union_data &>(*type.get());
}

smt_astt
smt_convt::mk_tuple_symbol(const expr2tc &expr)
{
  // Assuming this is a symbol, convert it to being an ast with tuple type.
  // That's done by creating the prefix for the names of all the contained
  // variables, and storing it.
  const symbol2t &sym = to_symbol2t(expr);
  std::string name = sym.get_symbol_name();

  // Because this tuple flattening doesn't join tuples through the symbol
  // table, there are some special names that need to be intercepted.
  if (name == "0" || name == "NULL")
    return null_ptr_ast;
  else if (name == "INVALID")
    return invalid_ptr_ast;

  // We put a '.' on the end of all symbols to deliminate the rest of the
  // name. However, these names may become expressions again, then be converted
  // again, thus accumulating dots. So don't.
  if (name[name.size() - 1] != '.')
    name += ".";

  smt_sortt sort = convert_sort(sym.type);
  assert(sort->id != SMT_SORT_ARRAY);
  return new tuple_smt_ast(this, sort, name);
}

smt_astt
smt_convt::mk_tuple_array_symbol(const expr2tc &expr)
{
  // Exactly the same as creating a tuple symbol, but for arrays.
  const symbol2t &sym = to_symbol2t(expr);
  std::string name = sym.get_symbol_name() + "[]";
  smt_sortt sort = convert_sort(sym.type);
  return new array_smt_ast(this, sort, name);
}

smt_astt 
smt_convt::tuple_array_create(const type2tc &array_type,
                              smt_astt *inputargs,
                              bool const_array,
                              smt_sortt domain __attribute__((unused)))
{
  // Create a tuple array from a constant representation. This means that
  // either we have an array_of or a constant_array. Handle this by creating
  // a fresh tuple array symbol, then repeatedly updating it with tuples at each
  // index. Ignore infinite arrays, they're "not for you".
  // XXX - probably more efficient to update each member array, but not now.
  smt_sortt sort = convert_sort(array_type);
  std::string name = mk_fresh_name("tuple_array_create::") + ".";
  smt_astt newsym = new array_smt_ast(this, sort, name);

  // Check size
  const array_type2t &arr_type = to_array_type(array_type);
  if (arr_type.size_is_infinite) {
    // Guarentee nothing, this is modelling only.
    return newsym;
  } else if (!is_constant_int2t(arr_type.array_size)) {
    std::cerr << "Non-constant sized array of type constant_array_of2t"
              << std::endl;
    abort();
  }

  const constant_int2t &thesize = to_constant_int2t(arr_type.array_size);
  uint64_t sz = thesize.constant_value.to_ulong();

  if (const_array) {
    // Repeatedly store the same value into this at all the demanded
    // indexes.
    smt_astt init = inputargs[0];
    for (unsigned int i = 0; i < sz; i++) {
      newsym = newsym->update(this, init, i);
    }

    return newsym;
  } else {
    // Repeatedly store operands into this.
    for (unsigned int i = 0; i < sz; i++) {
      newsym = newsym->update(this, inputargs[i], i);
    }

    return newsym;
  }
}

expr2tc
smt_convt::tuple_get(const expr2tc &expr)
{
  assert(is_symbol2t(expr) && "Non-symbol in smtlib expr get()");
  const symbol2t &sym = to_symbol2t(expr);
  std::string name = sym.get_symbol_name();

  tuple_smt_astt a = to_tuple_ast(convert_ast(expr));
  return tuple_get_rec(a);
}

expr2tc
smt_convt::tuple_get_rec(tuple_smt_astt tuple)
{
  tuple_smt_sortt sort = to_tuple_sort(tuple->sort);

  // XXX - what's the correct type to return here.
  constant_struct2tc outstruct(sort->thetype, std::vector<expr2tc>());
  const struct_union_data &strct = get_type_def(sort->thetype);

  // If this tuple was free and never read, don't attempt to extract data from
  // it. There isn't any.
  if (tuple->elements.size() == 0) {
    forall_types(it, strct.members) {
      outstruct.get()->datatype_members.push_back(expr2tc());
    }
    return outstruct;
  }

  // Run through all fields and despatch to 'get' again.
  unsigned int i = 0;
  forall_types(it, strct.members) {
    expr2tc res;

    if (is_tuple_ast_type(*it)) {
      res = tuple_get_rec(to_tuple_ast(tuple->elements[i]));
    } else if (is_tuple_array_ast_type(*it)) {
      res = expr2tc(); // XXX currently unimplemented
    } else if (is_number_type(*it)) {
      res = get_bv(*it, tuple->elements[i]);
    } else if (is_bool_type(*it)) {
      res = get_bool(tuple->elements[i]);
    } else if (is_array_type(*it)) {
      std::cerr << "Fetching array elements inside tuples currently unimplemented, sorry" << std::endl;
      res = expr2tc();
    } else {
      std::cerr << "Unexpected type in tuple_get_rec" << std::endl;
      abort();
    }

    outstruct.get()->datatype_members.push_back(res);
    i++;
  }

  // If it's a pointer, rewrite.
  if (is_pointer_type(sort->thetype) || sort->thetype == pointer_struct) {
    uint64_t num = to_constant_int2t(outstruct->datatype_members[0])
                                    .constant_value.to_uint64();
    uint64_t offs = to_constant_int2t(outstruct->datatype_members[1])
                                     .constant_value.to_uint64();
    pointer_logict::pointert p(num, BigInt(offs));
    return pointer_logic.back().pointer_expr(p,
                                 type2tc(new pointer_type2t(get_empty_type())));
  }

  return outstruct;
}

expr2tc
smt_convt::tuple_array_get(const expr2tc &expr __attribute__((unused)))
{
  std::cerr << "Tuple array get currently unimplemented" << std::endl;
  return expr2tc();
}

smt_astt 
smt_convt::array_create(const expr2tc &expr)
{
  if (is_constant_array_of2t(expr))
    return convert_array_of_prep(expr);

  // Handle constant array expressions: these don't have tuple type and so
  // don't need funky handling, but we need to create a fresh new symbol and
  // repeatedly store the desired data into it, to create an SMT array
  // representing the expression we're converting.
  std::string name = mk_fresh_name("array_create::") + ".";
  expr2tc newsym = symbol2tc(expr->type, name);

  // Check size
  const array_type2t &arr_type = to_array_type(expr->type);
  if (arr_type.size_is_infinite) {
    // Guarentee nothing, this is modelling only.
    return convert_ast(newsym);
  } else if (!is_constant_int2t(arr_type.array_size)) {
    std::cerr << "Non-constant sized array of type constant_array_of2t"
              << std::endl;
    abort();
  }

  const constant_int2t &thesize = to_constant_int2t(arr_type.array_size);
  uint64_t sz = thesize.constant_value.to_ulong();

  assert(is_constant_array2t(expr));
  const constant_array2t &array = to_constant_array2t(expr);

  // Repeatedly store things into this.
  smt_astt newsym_ast = convert_ast(newsym);
  for (unsigned int i = 0; i < sz; i++) {
    expr2tc init = array.datatype_members[i];

    // Workaround for bools-in-arrays
    if (is_bool_type(array.datatype_members[i]->type) && !int_encoding &&
        no_bools_in_arrays)
      init = typecast2tc(type2tc(new unsignedbv_type2t(1)), init);

    newsym_ast = newsym_ast->update(this, convert_ast(init), i);
  }

  return newsym_ast;
}

smt_astt 
smt_convt::convert_array_of_prep(const expr2tc &expr)
{
  const constant_array_of2t &arrof = to_constant_array_of2t(expr);
  const array_type2t &arrtype = to_array_type(arrof.type);
  expr2tc base_init;
  unsigned long array_size = 0;

  // So: we have an array_of, that we have to convert into a bunch of stores.
  // However, it might be a nested array. If that's the case, then we're
  // guarenteed to have another array_of in the initializer (which in turn might
  // be nested). In that case, flatten to a single array of whatever's at the
  // bottom of the array_of.
  if (is_array_type(arrtype.subtype)) {
    type2tc flat_type = flatten_array_type(expr->type);
    const array_type2t &arrtype2 = to_array_type(flat_type);
    array_size = calculate_array_domain_width(arrtype2);

    expr2tc rec_expr = expr;
    while (is_constant_array_of2t(rec_expr))
      rec_expr = to_constant_array_of2t(rec_expr).initializer;

    base_init = rec_expr;
  } else {
    base_init = arrof.initializer;
    array_size = calculate_array_domain_width(arrtype);
  }

  if (is_structure_type(base_init->type))
    return tuple_array_of(base_init, array_size);
  else if (is_pointer_type(base_init->type))
    return pointer_array_of(base_init, array_size);
  else
    return convert_array_of(base_init, array_size);
}

smt_astt 
smt_convt::convert_array_of(const expr2tc &init_val, unsigned long array_size)
{
  // We now an initializer, and a size of array to build. So:

  std::vector<expr2tc> array_of_inits;
  for (unsigned long i = 0; i < (1ULL << array_size); i++)
    array_of_inits.push_back(init_val);

  constant_int2tc real_arr_size(index_type2(), BigInt(1ULL << array_size));
  type2tc newtype(new array_type2t(init_val->type, real_arr_size, false));

  expr2tc res(new constant_array2t(newtype, array_of_inits));
  return convert_ast(res);
}

smt_astt 
smt_convt::tuple_array_of(const expr2tc &init_val, unsigned long array_size)
{
  assert(!tuple_support);

  // An array of tuples without tuple support: decompose into array_of's each
  // subtype.
  const struct_union_data &subtype =  get_type_def(init_val->type);
  const constant_datatype_data &data =
    static_cast<const constant_datatype_data &>(*init_val.get());

  constant_int2tc arrsize(index_type2(), BigInt(array_size));
  type2tc arrtype(new array_type2t(init_val->type, arrsize, false));
  std::string name = mk_fresh_name("tuple_array_of::") + ".";
  symbol2tc tuple_arr_of_sym(arrtype, irep_idt(name));

  smt_sortt sort = convert_sort(arrtype);
  smt_astt newsym = new array_smt_ast(this, sort, name);

  assert(subtype.members.size() == data.datatype_members.size());
  for (unsigned long i = 0; i < subtype.members.size(); i++) {
    const expr2tc &val = data.datatype_members[i];
    type2tc subarr_type = type2tc(new array_type2t(val->type, arrsize, false));
    constant_array_of2tc sub_array_of(subarr_type, val);

    smt_astt tuple_arr_of_sym_ast = convert_ast(tuple_arr_of_sym);
    smt_astt target_array = tuple_arr_of_sym_ast->project(this, i);

    smt_astt sub_array_of_ast = convert_ast(sub_array_of);
    assert_ast(target_array->eq(this, sub_array_of_ast));
  }

  return newsym;
}

smt_astt 
smt_convt::pointer_array_of(const expr2tc &init_val, unsigned long array_width)
{
  // Actually a tuple, but the operand is going to be a symbol, null.
  assert(is_symbol2t(init_val) && "Pointer type'd array_of can only be an "
         "array of null");
  const symbol2t &sym = to_symbol2t(init_val);
  assert(sym.thename == "NULL" && "Pointer type'd array_of can only be an "
         "array of null");

  // Well known value; zero and zero.
  constant_int2tc zero_val(machine_ptr, BigInt(0));
  std::vector<expr2tc> operands;
  operands.reserve(2);
  operands.push_back(zero_val);
  operands.push_back(zero_val);

  constant_struct2tc strct(pointer_struct, operands);
  return tuple_array_of(strct, array_width);
}

smt_astt 
smt_convt::tuple_array_create_despatch(const expr2tc &expr,
                                       smt_sortt domain)
{
  // Take a constant_array2t or an array_of, and format the data from them into
  // a form palatable to tuple_array_create.

  if (is_constant_array_of2t(expr)) {
    const constant_array_of2t &arr = to_constant_array_of2t(expr);
    smt_astt arg = convert_ast(arr.initializer);

    return tuple_array_create(arr.type, &arg, true, domain);
  } else {
    assert(is_constant_array2t(expr));
    const constant_array2t &arr = to_constant_array2t(expr);
    smt_astt args[arr.datatype_members.size()];
    unsigned int i = 0;
    forall_exprs(it, arr.datatype_members) {
      args[i] = convert_ast(*it);
      i++;
    }

    return tuple_array_create(arr.type, args, false, domain);
  }
}

