#ifndef SOLIDITY_FRONTEND_SOLIDITY_CONVERT_H_
#define SOLIDITY_FRONTEND_SOLIDITY_CONVERT_H_

//#define __STDC_LIMIT_MACROS
//#define __STDC_FORMAT_MACROS

#include <memory>
#include <util/context.h>
#include <util/namespace.h>
#include <util/std_types.h>
#include <nlohmann/json.hpp>
#include <solidity-frontend/solidity_type.h>
#include <solidity-frontend/pattern_check.h>
#include <solidity-frontend/solidity_decl_tracker.h>

using varDeclTrackerPtr = std::shared_ptr<VarDeclTracker>&;
using funDeclTrackerPtr = std::shared_ptr<FunctionDeclTracker>&;

class solidity_convertert
{
public:
  solidity_convertert(
    contextt &_context,
    nlohmann::json &_ast_json,
    const std::string &_sol_func,
    const messaget &msg);
  virtual ~solidity_convertert() = default;

  bool convert();

protected:
  contextt &context;
  namespacet ns;
  nlohmann::json &ast_json;    // json for Solidity AST. Use vector for multiple contracts
  const std::string &sol_func; // Solidity function to be verified
  const messaget &msg;
  std::string absolute_path;
  std::vector< std::shared_ptr<VarDeclTracker> > global_vars; // vector to hold global variable declarations

  void print_json_element(nlohmann::json &json_in, const unsigned index,
    const std::string &key, const std::string& json_name);
  void print_json_array_element(nlohmann::json &json_in,
      const std::string& node_type, const unsigned index);

  unsigned int current_scope_var_num; // tracking scope while getting declarations

  std::shared_ptr<FunctionDeclTracker> current_functionDecl;

  bool convert_ast_nodes(nlohmann::json &contract_def);

  bool get_decl(nlohmann::json &ast_node, exprt &new_expr);
  bool get_var(varDeclTrackerPtr vd, exprt &new_expr);
  bool get_function(funDeclTrackerPtr &fd, exprt &new_expr);
  bool get_type(const QualTypeTracker &q_type, typet &new_type); // bool clang_c_convertert::get_type(const clang::QualType &q_type, typet &new_type)
  bool get_sub_type(const QualTypeTracker &q_type, typet &new_type); // bool clang_c_convertert::get_type(const clang::Type &the_type, typet &new_type)
  bool get_builtin_type(const QualTypeTracker &q_type, typet &new_type);
  void get_decl_name(const NamedDeclTracker &nd, std::string &name, std::string &id);
  bool generate_decl_usr(const NamedDeclTracker &nd, std::string &name, std::string &id);
  void get_location_from_decl(const SourceLocationTracker &decl_slm, locationt &location);
  void get_start_location_from_stmt(const StmtTracker* stmt, locationt &location);
  void get_final_location_from_stmt(const StmtTracker* stmt, locationt &location);
  void set_location(const SourceLocationTracker &decl_slm, std::string &function_name, locationt &location);
  bool get_binary_operator_expr(const BinaryOperatorTracker* binop, exprt &new_expr);
  std::string get_modulename_from_path(std::string path);
  std::string get_filename_from_path(std::string path);

  bool get_expr(const StmtTracker* stmt, exprt &new_expr);
  bool get_cast_expr(const ImplicitCastExprTracker* cast, exprt &new_expr);

  void get_default_symbol(
    symbolt &symbol,
    std::string module_name,
    typet type,
    std::string name,
    std::string id,
    locationt location);

  symbolt *move_symbol_to_context(symbolt &symbol);
  void convert_expression_to_code(exprt &expr);
  std::shared_ptr<VarDeclTracker> get_matched_decl_ref(unsigned ref_id); // function to get matched decl ref for DeclRefExpr
  bool get_decl_ref(const DeclRefExprTracker* dcl, exprt &new_expr);

  bool convert_integer_literal(
    const IntegerLiteralTracker* integer_literal,
    exprt &dest);
};

#endif /* SOLIDITY_FRONTEND_SOLIDITY_CONVERT_H_ */