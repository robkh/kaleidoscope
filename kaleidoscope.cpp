// Use this macro to generate DWARF
//#define __GENERATE_DWARF

#ifndef __GENERATE_DWARF
// Use this macro to turn on Command Line Interface
#define __USE_CLI

// Use this macro to turn on FPM optimizations
//#define __USE_FPM_PASSES
#endif

#ifdef __USE_CLI
// Use this macro to use JIT to evaluate the code, otherwise produce an object
// code.
#define __USE_JIT
#endif

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include <map>
#include <string>
#include <vector>

#ifdef __USE_JIT
#include "KaleidoscopeJIT.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#else
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#endif

#ifdef __GENERATE_DWARF
#include "llvm/IR/DIBuilder.h"
#endif

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5,

  // control
  tok_if = -6,
  tok_then = -7,
  tok_else = -8,
  tok_for = -9,
  tok_in = -10,

  // operators
  tok_binary = -11,
  tok_unary = -12,

  // var definition
  tok_var = -13,
};

static std::string IdentifierStr; // Filled in if tok_identifier
static double NumVal;             // Filled in if tok_number

struct SourceLocation {
  int Line;
  int Col;
};
static SourceLocation CurLoc;
static SourceLocation LexLoc = {1, 0};

static int advance() {
  int LastChar = getchar();

  if (LastChar == '\n' || LastChar == '\r') {
    LexLoc.Line++;
    LexLoc.Col = 0;
  } else
    LexLoc.Col++;
  return LastChar;
}

static int gettok() {
  static int LastChar = ' ';

  // Skip any whitespace.
  while (isspace(LastChar))
    LastChar = advance();

  if (isalpha(LastChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
    IdentifierStr = LastChar;
    while (isalnum((LastChar = advance())))
      IdentifierStr += LastChar;

    if (IdentifierStr == "def")
      return tok_def;
    if (IdentifierStr == "extern")
      return tok_extern;
    if (IdentifierStr == "if")
      return tok_if;
    if (IdentifierStr == "then")
      return tok_then;
    if (IdentifierStr == "else")
      return tok_else;
    if (IdentifierStr == "for")
      return tok_for;
    if (IdentifierStr == "in")
      return tok_in;
    if (IdentifierStr == "binary")
      return tok_binary;
    if (IdentifierStr == "unary")
      return tok_unary;
    if (IdentifierStr == "var")
      return tok_var;
    return tok_identifier;
  }

  if (isdigit(LastChar) || LastChar == '.') { // Number: [0-9.]+ FIXME: This
                                              // will incorrectly read
                                              // "1.23.45.67"
    std::string NumStr;
    do {
      NumStr += LastChar;
      LastChar = advance();
    } while (isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), 0);
    return tok_number;
  }

  if (LastChar == '#') {
    // Comment until EOF
    do
      LastChar = advance();
    while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if (LastChar != EOF)
      return gettok();
  }

  // Check for end of file. Don't eat the EOF.
  if (LastChar == EOF)
    return tok_eof;

  // Otherwise, jsut return the character as its ascii value.
  int ThisChar = LastChar;
  LastChar = advance();
  return ThisChar;
}

class ExprAST;
static llvm::LLVMContext TheContext;
static llvm::IRBuilder<> Builder(TheContext);
#ifdef __GENERATE_DWARF
struct DebugInfo {
  llvm::DICompileUnit *TheCU;
  llvm::DIType *DblTy;
  std::vector<llvm::DIScope *> LexicalBlocks;

  void emitLocation(ExprAST *AST);
  llvm::DIType *getDoubleTy();
} KSDbgInfo;

#endif

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//

llvm::raw_ostream &indent(llvm::raw_ostream &O, int size) {
  return O << std::string(size, ' ');
}

/// ExprAST - Base class for all expression nodes.
class ExprAST {
  SourceLocation Loc;

public:
  ExprAST(SourceLocation Loc = CurLoc) : Loc(Loc) {}
  virtual ~ExprAST() {}
  virtual llvm::Value *codegen() = 0;
  int getLine() const { return Loc.Line; }
  int getCol() const { return Loc.Col; }
  virtual llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) {
    return out << ':' << getLine() << ':' << getCol() << '\n';
  }
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(SourceLocation Loc, double Val) : ExprAST(Loc), Val(Val) {}
  llvm::Value *codegen() override;

  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
    return ExprAST::dump(out << Val, ind);
  }
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(SourceLocation Loc, const std::string &Name)
      : ExprAST(Loc), Name(Name) {}
  llvm::Value *codegen() override;
  const std::string &getName() const { return Name; }
  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
    return ExprAST::dump(out << Name, ind);
  }
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(SourceLocation Loc, char op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(Loc), Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  llvm::Value *codegen() override;
  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
    ExprAST::dump(out << "binary" << Op, ind);
    LHS->dump(indent(out, ind) << "LHS:", ind + 1);
    RHS->dump(indent(out, ind) << "RHS:", ind + 1);
    return out;
  }
};

/// UnaryExprAST - Expression class for a unary operator.
class UnaryExprAST : public ExprAST {
  char Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  UnaryExprAST(SourceLocation Loc, char Opcode,
               std::unique_ptr<ExprAST> Operand)
      : ExprAST(Loc), Opcode(Opcode), Operand(std::move(Operand)) {}

  llvm::Value *codegen() override;
  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
    ExprAST::dump(out << "unary" << Opcode, ind);
    Operand->dump(out, ind + 1);
    return out;
  }
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(SourceLocation Loc, const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : ExprAST(Loc), Callee(Callee), Args(std::move(Args)) {}
  llvm::Value *codegen() override;
  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
    ExprAST::dump(out << "call " << Callee, ind);
    for (const auto &Arg : Args)
      Arg->dump(indent(out, ind + 1), ind + 1);
    return out;
  }
};

/// IfExprAST - Expression class for if/then/else.
class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

public:
  IfExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Cond,
            std::unique_ptr<ExprAST> Then, std::unique_ptr<ExprAST> Else)
      : ExprAST(Loc), Cond(std::move(Cond)), Then(std::move(Then)),
        Else(std::move(Else)) {}

  llvm::Value *codegen() override;
  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
    ExprAST::dump(out << "if", ind);
    Cond->dump(indent(out, ind) << "Cond:", ind + 1);
    Then->dump(indent(out, ind) << "Then:", ind + 1);
    Else->dump(indent(out, ind) << "Else:", ind + 1);
    return out;
  }
};

/// ForExprAST - Expression class for for/in.
class ForExprAST : public ExprAST {
  std::string VarName;
  std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
  ForExprAST(SourceLocation Loc, const std::string &VarName,
             std::unique_ptr<ExprAST> Start, std::unique_ptr<ExprAST> End,
             std::unique_ptr<ExprAST> Step, std::unique_ptr<ExprAST> Body)
      : ExprAST(Loc), VarName(VarName), Start(std::move(Start)),
        End(std::move(End)), Step(std::move(Step)), Body(std::move(Body)) {}

  llvm::Value *codegen() override;

  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
    ExprAST::dump(out << "for", ind);
    Start->dump(indent(out, ind) << "Start:", ind + 1);
    End->dump(indent(out, ind) << "End:", ind + 1);
    Step->dump(indent(out, ind) << "Step:", ind + 1);
    Body->dump(indent(out, ind) << "Body:", ind + 1);
    return out;
  }
};

/// VarExprAST - Expression class for var/in
class VarExprAST : public ExprAST {
  std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;
  std::unique_ptr<ExprAST> Body;

public:
  VarExprAST(
      SourceLocation Loc,
      std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames,
      std::unique_ptr<ExprAST> Body)
      : ExprAST(Loc), VarNames(std::move(VarNames)), Body(std::move(Body)) {}

  llvm::Value *codegen() override;
  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
    ExprAST::dump(out << "var", ind);
    for (const auto &NamedVar : VarNames)
      NamedVar.second->dump(indent(out, ind) << NamedVar.first << ':', ind + 1);
    Body->dump(indent(out, ind) << "Body:", ind + 1);
    return out;
  }
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the
/// number of arguments the function takes).
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;
  bool IsOperator;
  unsigned Precedence; // precedence if a binary op
  int Line;

public:
  PrototypeAST(SourceLocation Loc, const std::string &Name,
               std::vector<std::string> Args, bool IsOperator = false,
               unsigned Prec = 0)
      : Name(Name), Args(std::move(Args)), IsOperator(IsOperator),
        Precedence(Prec), Line(Loc.Line) {}
  llvm::Function *codegen();

  const std::string &getName() const { return Name; }
  const std::string &getArgName(int i) const { return Args[i]; }

  bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
  bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

  char getOperatorName() const {
    assert(isUnaryOp() || isBinaryOp());
    return Name[Name.size() - 1];
  }

  unsigned getBinaryPrecedence() const { return Precedence; }
  int getLine() const { return Line; }

  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) {
    indent(out, ind) << "PrototypeAST " << Name << "\n";
    ++ind;
    for (const auto &Arg : Args)
      indent(out, ind) << Arg << ':';
    return out;
  }
};

/// FunctionAST - This class represents a function definition itself.
class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}
  llvm::Function *codegen();

  llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) {
    indent(out, ind) << "FunctionAST\n";
    ++ind;
    Proto->dump(out, ind);
    indent(out, ind) << "Body:";
    return Body ? Body->dump(out, ind) : out << "null\n";
  }
};

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

/// CurTok/getNextToken - Provide a simple token buffer. CurTok is the current
/// token the parser is looking at. getNextToken reads another token from the
/// lexer and updates CurTok with its results.
static int CurTok;
static int getNextToken() { return CurTok = gettok(); }

/// LogError* - These are little helper functions for error handling.
std::unique_ptr<ExprAST> LogError(const char *Str) {
  fprintf(stderr, "LogError: %s\n", Str);
  return nullptr;
}
std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
  LogError(Str);
  return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  SourceLocation NumLoc = CurLoc;
  auto Result = std::make_unique<NumberExprAST>(NumLoc, NumVal);
  getNextToken();
  return std::move(Result);
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // eat '('
  auto V = ParseExpression();
  if (!V)
    return nullptr;

  if (CurTok != ')')
    return LogError("Expected ')'");
  getNextToken(); // eat ')'
  return std::move(V);
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;

  SourceLocation LitLoc = CurLoc;

  getNextToken();

  if (CurTok != '(') // Simple variable ref.
    return std::make_unique<VariableExprAST>(LitLoc, IdName);

  // Call.
  getNextToken();
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (1) {
      if (auto Arg = ParseExpression())
        Args.push_back(std::move(Arg));
      else
        return nullptr;

      if (CurTok == ')')
        break;

      if (CurTok != ',')
        return LogError("Expected ')' or ',' in argument list");
      getNextToken();
    }
  }

  // Eat the ')'.
  getNextToken();

  return std::make_unique<CallExprAST>(LitLoc, IdName, std::move(Args));
}

/// ifexpr ::= 'if' expression 'then' expression 'else' expression
static std::unique_ptr<ExprAST> ParseIfExpr() {
  SourceLocation IfLoc = CurLoc;
  getNextToken(); // eat 'if'

  // condition
  auto Cond = ParseExpression();
  if (!Cond)
    return nullptr;

  if (CurTok != tok_then)
    return LogError("Expected then");
  getNextToken(); // eat 'then'

  auto Then = ParseExpression();
  if (!Then)
    return nullptr;

  if (CurTok != tok_else)
    return LogError("Expected else");

  getNextToken(); // eat 'else'

  auto Else = ParseExpression();
  if (!Else)
    return nullptr;

  return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then),
                                     std::move(Else));
}

/// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
  SourceLocation ForLoc = CurLoc;
  getNextToken(); // eat 'for'

  if (CurTok != tok_identifier)
    return LogError("Expected identifier after for");

  std::string IdName = IdentifierStr;
  getNextToken(); // eat identifier

  if (CurTok != '=')
    return LogError("Expected '='");
  getNextToken(); // eat '='

  auto Start = ParseExpression();
  if (!Start)
    return nullptr;
  if (CurTok != ',')
    return LogError("Expected ',' after the start value");
  getNextToken(); // eat ','

  auto End = ParseExpression();
  if (!End)
    return nullptr;

  // The step value is optional
  std::unique_ptr<ExprAST> Step;
  if (CurTok == ',') {
    getNextToken();
    Step = ParseExpression();
    if (!Step)
      return nullptr;
  }

  if (CurTok != tok_in)
    return LogError("Expected 'in' after for");
  getNextToken(); // eat 'in'

  auto Body = ParseExpression();
  if (!Body)
    return nullptr;

  return std::make_unique<ForExprAST>(ForLoc, IdName, std::move(Start),
                                      std::move(End), std::move(Step),
                                      std::move(Body));
}

/// varexpr ::= 'var' identifier ('=' expression)?
///                   (',' identififer ('=' expression)?)* 'in' expression
static std::unique_ptr<ExprAST> ParseVarExpr() {
  SourceLocation VarLoc = CurLoc;
  getNextToken(); // eat the var.

  std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

  // At least one variable is required.
  if (CurTok != tok_identifier)
    return LogError("Expected identififer after var");

  while (1) {
    std::string Name = IdentifierStr;
    getNextToken(); // eat identifier.

    // Read the optional initializer.
    std::unique_ptr<ExprAST> Init;
    if (CurTok == '=') {
      getNextToken(); // eat the '='.

      Init = ParseExpression();
      if (!Init)
        return nullptr;
    }

    VarNames.push_back(std::make_pair(Name, std::move(Init)));

    // End of vat list, exit loop.
    if (CurTok != ',')
      break;
    getNextToken(); // eat the ','.

    if (CurTok != tok_identifier)
      return LogError("Exprected identifier list after var");
  }

  // At this point, we have to have 'in'.
  if (CurTok != tok_in)
    return LogError("Expected 'in' keyword after 'var'");
  getNextToken(); // eat 'in'.

  auto Body = ParseExpression();
  if (!Body)
    return nullptr;

  return std::make_unique<VarExprAST>(VarLoc, std::move(VarNames),
                                      std::move(Body));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  default:
    return LogError("Unknown token when exprecting an expression");
  case tok_identifier:
    return ParseIdentifierExpr();
  case tok_number:
    return ParseNumberExpr();
  case '(':
    return ParseParenExpr();
  case tok_if:
    return ParseIfExpr();
  case tok_for:
    return ParseForExpr();
  case tok_var:
    return ParseVarExpr();
  }
}

/// BinopPrecedence - This holds the precedence for each binary operator that
/// is defined
static std::map<char, int> BinopPrecedence;

/// GetTokPrecedence - Get the precedence of the pending binary operator
/// token.
static int GetTokPrecedence() {
  if (!isascii(CurTok))
    return -1;

  // Make sure it's a declared binop.
  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0)
    return -1;
  return TokPrec;
}

/// unary
///   ::= primary
///   ::= unop unary
static std::unique_ptr<ExprAST> ParseUnary() {
  // If the current token is not an operator, it must be a primary expr.
  if (!isascii(CurTok) || CurTok == '(' || CurTok == ',')
    return ParsePrimary();

  SourceLocation UnLoc = CurLoc;
  // If this is an unary operator, read it.
  int Opc = CurTok;
  getNextToken();
  if (auto Operand = ParseUnary())
    return std::make_unique<UnaryExprAST>(UnLoc, Opc, std::move(Operand));
  return nullptr;
}

/// binoprhs
///   ::= (binop unary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
  // If this is a binop, find its precedence.
  while (1) {
    int TokPrec = GetTokPrecedence();

    // If this is a binop that binds at least as tightly as the current binop,
    // consume it, otherwise we are done.
    if (TokPrec < ExprPrec)
      return LHS;

    // Okay, we know this is a binop.
    int BinOp = CurTok;
    SourceLocation BinLoc = CurLoc;
    getNextToken(); // eat binop

    // Parse the primary expression after the binary operator.
    auto RHS = ParseUnary();
    if (!RHS)
      return nullptr;

    // If BinOp binds lesss tightly with RHS that the operator after RHS, let
    // the pending operator take RHS as its LHS.
    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    // Merge LHS/RHS.
    LHS = std::make_unique<BinaryExprAST>(BinLoc, BinOp, std::move(LHS),
                                          std::move(RHS));
  }
}

/// expression
///   ::= unary binoprhs
static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParseUnary();
  if (!LHS)
    return nullptr;

  return ParseBinOpRHS(0, std::move(LHS));
}

/// prototype
///   ::= id '(' id* ')'
///   ::= binary LETTER number? (id, id)
///   ::= unary LETTER (id)
static std::unique_ptr<PrototypeAST> ParsePrototype() {
  std::string FnName;

  SourceLocation FnLoc = CurLoc;

  unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary
  unsigned BinaryPrecedence = 30;

  switch (CurTok) {
  default:
    return LogErrorP("Expected function name in prototype");
  case tok_identifier:
    FnName = IdentifierStr;
    Kind = 0;
    getNextToken();
    break;
  case tok_binary:
    getNextToken();
    if (!isascii(CurTok))
      return LogErrorP("Expected binary operator");
    FnName = "binary";
    FnName += (char)CurTok;
    Kind = 2;
    getNextToken();

    // Read the precedence if present.
    if (CurTok == tok_number) {
      if (NumVal < 1 || NumVal > 100)
        return LogErrorP("Invalid precedence: must be 1..100");
      BinaryPrecedence = (unsigned)NumVal;
      getNextToken();
    }
    break;
  case tok_unary:
    getNextToken();
    if (!isascii(CurTok))
      return LogErrorP("Expected unary operator");
    FnName = "unary";
    FnName += (char)CurTok;
    Kind = 1;
    getNextToken();
    break;
  }

  if (CurTok != '(')
    return LogErrorP("Exprected '(' in prototype");

  // Read the list of arguments names.
  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier)
    ArgNames.push_back(IdentifierStr);
  if (CurTok != ')')
    return LogErrorP("Expected ')' in prototype");

  // success.
  getNextToken();

  // Verify right number of names for operator.
  if (Kind && ArgNames.size() != Kind)
    return LogErrorP("Invalid number of operands for opeator");

  return std::make_unique<PrototypeAST>(FnLoc, FnName, std::move(ArgNames),
                                        Kind != 0, BinaryPrecedence);
}

/// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken(); // eat 'def'
  auto Proto = ParsePrototype();
  if (!Proto)
    return nullptr;

  if (auto E = ParseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  return nullptr;
}

/// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextToken(); // eat 'extern'
  return ParsePrototype();
}

/// toplevelexpt ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  SourceLocation FnLoc = CurLoc;
  if (auto E = ParseExpression()) {
#ifdef __USE_CLI
    // Make an anonymous proto.
    auto Proto = std::make_unique<PrototypeAST>(FnLoc, "__anon_expr",
                                                std::vector<std::string>());
#else
    // Make an anonymous proto.
    auto Proto = std::make_unique<PrototypeAST>(FnLoc, "main",
                                                std::vector<std::string>());
#endif
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}

#ifdef __GENERATE_DWARF
//===----------------------------------------------------------------------===//
// Debug Info Support
//===----------------------------------------------------------------------===//
static std::unique_ptr<llvm::DIBuilder> DBuilder;

llvm::DIType *DebugInfo::getDoubleTy() {
  if (DblTy)
    return DblTy;

  DblTy = DBuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
  return DblTy;
}

static llvm::DISubroutineType *CreateFunctionType(unsigned NumArgs,
                                                  llvm::DIFile *Unit) {
  llvm::SmallVector<llvm::Metadata *, 8> EltTys;
  llvm::DIType *DblTy = KSDbgInfo.getDoubleTy();

  // Add the result type.
  EltTys.push_back(DblTy);

  for (unsigned i = 0, e = NumArgs; i != e; ++i)
    EltTys.push_back(DblTy);

  return DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray(EltTys));
}

void DebugInfo::emitLocation(ExprAST *AST) {
  if (!AST)
    return Builder.SetCurrentDebugLocation(llvm::DebugLoc());
  llvm::DIScope *Scope;
  if (LexicalBlocks.empty())
    Scope = TheCU;
  else
    Scope = LexicalBlocks.back();
  Builder.SetCurrentDebugLocation(
      llvm::DebugLoc::get(AST->getLine(), AST->getCol(), Scope));
}

#endif

//===----------------------------------------------------------------------===//
// Code generation to LLVM IR
//===----------------------------------------------------------------------===//
static std::unique_ptr<llvm::Module> TheModule;
static std::map<std::string, llvm::AllocaInst *> NamedValues;
#ifdef __USE_FPM_PASSES
static std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM;
#endif
#ifdef __USE_JIT
static std::unique_ptr<llvm::orc::KaleidoscopeJIT> TheJIT;
#endif
static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

llvm::Value *LogErrorV(const char *Str) {
  LogError(Str);
  return nullptr;
}

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block
/// of the function. This is used for mutable variables etc.
static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                                const std::string &VarName) {
  llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(llvm::Type::getDoubleTy(TheContext), 0,
                           VarName.c_str());
}

llvm::Function *getFunction(std::string Name) {
  // First, see if the function has already been added to the current module.
  if (auto *F = TheModule->getFunction(Name))
    return F;

  // If not, check whether we can codegen the declaration from some existing
  // prototype.
  auto FI = FunctionProtos.find(Name);
  if (FI != FunctionProtos.end())
    return FI->second->codegen();

  // If no existing prototype exists, return null.
  return nullptr;
}

llvm::Value *NumberExprAST::codegen() {
#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(this);
#endif
  return llvm::ConstantFP::get(TheContext, llvm::APFloat(Val));
}

llvm::Value *VariableExprAST::codegen() {
  // Look this variable up in the function.
  llvm::Value *V = NamedValues[Name];
  if (!V)
    LogErrorV("Unknown variable name");

#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(this);
#endif
  // Load the value.
  return Builder.CreateLoad(V, Name.c_str());
}

llvm::Value *UnaryExprAST::codegen() {
  llvm::Value *OperandV = Operand->codegen();
  if (!OperandV)
    return nullptr;

  llvm::Function *F = getFunction(std::string("unary") + Opcode);
  if (!F)
    return LogErrorV("Unknown unary operator");

#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(this);
#endif
  return Builder.CreateCall(F, OperandV, "unop");
}

llvm::Value *BinaryExprAST::codegen() {
#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(this);
#endif
  // Special case '=' because we don't want to emit the LHS as an expression.
  if (Op == '=') {
    // Assignment requires the LHS to be an identifier.
    VariableExprAST *LHSE = dynamic_cast<VariableExprAST *>(LHS.get());
    if (!LHSE)
      return LogErrorV("Destination of '=' must be a variable");

    // Codegen the RHS.
    llvm::Value *Val = RHS->codegen();
    if (!Val)
      return nullptr;

    // Look up the name.
    llvm::AllocaInst *Alloca = NamedValues[LHSE->getName()];
    if (!Alloca)
      return LogErrorV("Unknown variable name");

    Builder.CreateStore(Val, Alloca);
    return Val;
  }

  llvm::Value *L = LHS->codegen();
  llvm::Value *R = RHS->codegen();
  if (!L || !R)
    return nullptr;

  switch (Op) {
  case '+':
    return Builder.CreateFAdd(L, R, "addtmp");
  case '-':
    return Builder.CreateFSub(L, R, "subtmp");
  case '*':
    return Builder.CreateFMul(L, R, "multmp");
  case '<':
    L = Builder.CreateFCmpULT(L, R, "cmptmp");
    // Convert bool 0/1 to double 0.0/1.0
    return Builder.CreateUIToFP(L, llvm::Type::getDoubleTy(TheContext),
                                "booltmp");
  default:
    break;
  }

  // If it was not a builtin binary operator, it must be a user defined one.
  // Emit a call to it.
  llvm::Function *F = getFunction(std::string("binary") + Op);
  if (!F)
    return LogErrorV("Binary operator not found!");

  llvm::Value *Ops[2] = {L, R};
  return Builder.CreateCall(F, Ops, "binop");
}

llvm::Value *CallExprAST::codegen() {
#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(this);
#endif
  llvm::Function *CalleeF = getFunction(Callee);
  if (!CalleeF)
    return LogErrorV("Unknown function referenced");

  // If argument mismatch error.
  if (CalleeF->arg_size() != Args.size())
    return LogErrorV("Incorrect # arguments passed");

  std::vector<llvm::Value *> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->codegen());
    if (!ArgsV.back())
      return nullptr;
  }

  return Builder.CreateCall(CalleeF, ArgsV, "calltmp");
}

llvm::Value *IfExprAST::codegen() {
#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(this);
#endif
  llvm::Value *CondV = Cond->codegen();

  if (!CondV)
    return nullptr;

  // Convert condition to a bool by comparing non-equal to 0.0
  CondV = Builder.CreateFCmpONE(
      CondV, llvm::ConstantFP::get(TheContext, llvm::APFloat(0.0)), "ifcond");

  llvm::Function *TheFunction = Builder.GetInsertBlock()->getParent();

  // Create basic blocks for 'then' and 'else' cases. Insert the 'then' block
  // at the end of the function.
  llvm::BasicBlock *ThenBB =
      llvm::BasicBlock::Create(TheContext, "then", TheFunction);
  llvm::BasicBlock *ElseBB = llvm::BasicBlock::Create(TheContext, "else");
  llvm::BasicBlock *MergeBB = llvm::BasicBlock::Create(TheContext, "ifcont");

  Builder.CreateCondBr(CondV, ThenBB, ElseBB);

  // Emit then value
  Builder.SetInsertPoint(ThenBB);

  llvm::Value *ThenV = Then->codegen();
  if (!ThenV)
    return nullptr;

  Builder.CreateBr(MergeBB);
  // Codegen of 'then' can change the current block, update ThenBB for the
  // PHI.
  ThenBB = Builder.GetInsertBlock();

  // Emit else value
  TheFunction->getBasicBlockList().push_back(ElseBB);
  Builder.SetInsertPoint(ElseBB);

  llvm::Value *ElseV = Else->codegen();
  if (!ElseV)
    return nullptr;

  Builder.CreateBr(MergeBB);
  // Update ElseBB for the PHI.
  ElseBB = Builder.GetInsertBlock();

  // Emit merge
  TheFunction->getBasicBlockList().push_back(MergeBB);
  Builder.SetInsertPoint(MergeBB);
  llvm::PHINode *PN =
      Builder.CreatePHI(llvm::Type::getDoubleTy(TheContext), 2, "iftmp");

  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}

llvm::Value *ForExprAST::codegen() {
  // Make a new basic block for the loop header.
  llvm::Function *TheFunction = Builder.GetInsertBlock()->getParent();

  // Create an alloca fot the variable in the entry block.
  llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);

#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(this);
#endif

  // Emit the start code, without a variable in the scope.
  llvm::Value *StartVal = Start->codegen();
  if (!StartVal)
    return nullptr;

  // Store the value into the alloca.
  Builder.CreateStore(StartVal, Alloca);

  llvm::BasicBlock *LoopBB =
      llvm::BasicBlock::Create(TheContext, "loop", TheFunction);

  // Insert a branch to the loop.
  Builder.CreateBr(LoopBB);

  // Start insertion in the loop.
  Builder.SetInsertPoint(LoopBB);

  // Within the loop, the variable is defined equal to the PHI node. If it
  // shadows an existing variable, we have to restore it.
  llvm::AllocaInst *OldVal = NamedValues[VarName];
  NamedValues[VarName] = Alloca;

  // Emit the body of the Loop. This can change the current BB.
  if (!Body->codegen())
    return nullptr;

  // Emit the step value.
  llvm::Value *StepVal = nullptr;
  if (Step) {
    StepVal = Step->codegen();
    if (!StepVal)
      return nullptr;
  } else {
    // if not specified, use 1.0.
    StepVal = llvm::ConstantFP::get(TheContext, llvm::APFloat(1.0));
  }

  // Compute the end condition.
  llvm::Value *EndCond = End->codegen();
  if (!EndCond)
    return nullptr;

  // Reload, increment, and restore the alloca. This handles the case where
  // the body of the loop mutates the variable.
  llvm::Value *CurVar = Builder.CreateLoad(Alloca);
  llvm::Value *NextVar = Builder.CreateFAdd(CurVar, StepVal, "nextvar");
  Builder.CreateStore(NextVar, Alloca);

  // Convert condition to a bool by comparing non-equal to 0.0.
  EndCond = Builder.CreateFCmpONE(
      EndCond, llvm::ConstantFP::get(TheContext, llvm::APFloat(0.0)),
      "loopcond");

  // Create the "after loop" block and instert it.
  llvm::BasicBlock *AfterLoopBB =
      llvm::BasicBlock::Create(TheContext, "afterloop", TheFunction);

  // Insert the conditional branch into the end of the LoopEndBB
  Builder.CreateCondBr(EndCond, LoopBB, AfterLoopBB);

  // New code will be inserted in AfterBB.
  Builder.SetInsertPoint(AfterLoopBB);

  // Restore the unshadowed variable
  if (OldVal)
    NamedValues[VarName] = OldVal;
  else
    NamedValues.erase(VarName);

  // for expr always returns 0.0.
  return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(TheContext));
}

llvm::Value *VarExprAST::codegen() {
  std::vector<llvm::AllocaInst *> OldBindings;

  llvm::Function *TheFunction = Builder.GetInsertBlock()->getParent();

#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(this);
#endif

  // Register all variables and emit their initializer.
  for (unsigned i = 0, e = VarNames.size(); i != e; ++i) {
    const std::string &VarName = VarNames[i].first;
    ExprAST *Init = VarNames[i].second.get();

    // Emit the initializer before adding the variable to scope, this prevents
    // the initializer from referencing the variable itself, and permits stuff
    // like this:
    //  var a = 1 in
    //    var a = a in ... # refers to outer 'a'.
    llvm::Value *InitVal;
    if (Init) {
      InitVal = Init->codegen();
      if (!InitVal)
        return nullptr;
    } else {
      // if not specified, use 0.0.
      InitVal = llvm::ConstantFP::get(TheContext, llvm::APFloat(0.0));
    }

    llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);
    Builder.CreateStore(InitVal, Alloca);

    // Remember the old variable binding so that we car restore the binding
    // when we unrecurse.
    OldBindings.push_back(NamedValues[VarName]);

    // Remember this binding.
    NamedValues[VarName] = Alloca;
  }

  // Codegen the body, now that all vars are in scope.
  llvm::Value *BodyVal = Body->codegen();
  if (!BodyVal)
    return nullptr;

  // Pop all our variables from scope.
  for (unsigned i = 0, e = VarNames.size(); i != e; ++i)
    NamedValues[VarNames[i].first] = OldBindings[i];

  // Return the body computation.
  return BodyVal;
}

llvm::Function *PrototypeAST::codegen() {
  // Make the function type: double(double,double) etc.
  std::vector<llvm::Type *> Doubles(Args.size(),
                                    llvm::Type::getDoubleTy(TheContext));
  llvm::FunctionType *FT = llvm::FunctionType::get(
      llvm::Type::getDoubleTy(TheContext), Doubles, false);

  llvm::Function *F = llvm::Function::Create(
      FT, llvm::Function::ExternalLinkage, Name, TheModule.get());

  return F;
}

llvm::Function *FunctionAST::codegen() {
  // Transfer ownership of the protoype to the FunctionProtos map, but keep a
  // reference to it for use below.
  auto &P = *Proto;
  FunctionProtos[Proto->getName()] = std::move(Proto);
  llvm::Function *TheFunction = getFunction(P.getName());

  if (!TheFunction)
    return nullptr;

  if (!TheFunction->empty())
    return (llvm::Function *)LogErrorV("Function cannot be redefined");

  // if this is an operator, install it.
  if (P.isBinaryOp())
    BinopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();

  // Set names for all arguments.
  unsigned Idx = 0;
  for (auto &Arg : TheFunction->args())
    Arg.setName(P.getArgName(Idx++));

  // Create a new basic block to start instertion into.
  llvm::BasicBlock *BB =
      llvm::BasicBlock::Create(TheContext, "entry", TheFunction);
  Builder.SetInsertPoint(BB);

#ifdef __GENERATE_DWARF
  llvm::DIFile *Unit = DBuilder->createFile(KSDbgInfo.TheCU->getFilename(),
                                            KSDbgInfo.TheCU->getDirectory());

  llvm::DIScope *FContext = Unit;
  unsigned LineNo = 0;
  unsigned ScopeLine = 0;
  llvm::DISubprogram *SP = DBuilder->createFunction(
      FContext, P.getName(), llvm::StringRef(), Unit, LineNo,
      CreateFunctionType(TheFunction->arg_size(), Unit),
      false /* internal linkage */, true /*definition */, ScopeLine,
      llvm::DINode::FlagPrototyped, false);
  TheFunction->setSubprogram(SP);

  // Push the current scope.
  KSDbgInfo.LexicalBlocks.push_back(SP);

  // Unset the location for the prologue emission (leading instructions with no
  // location in a function are considered part of the prologue and the debugger
  // will run past them when breaking on a function)
  KSDbgInfo.emitLocation(nullptr);
#endif

  // Record the function arguments in the NamedValues map.
  NamedValues.clear();
  unsigned ArgIdx = 0;
  for (auto &Arg : TheFunction->args()) {
    // Create an alloca for this variable.
    llvm::AllocaInst *Alloca =
        CreateEntryBlockAlloca(TheFunction, Arg.getName());

#ifdef __GENERATE_DWARF
    // Create a denug descriptor for the variable.
    llvm::DILocalVariable *D = DBuilder->createParameterVariable(
        SP, Arg.getName(), ++ArgIdx, Unit, LineNo, KSDbgInfo.getDoubleTy(),
        true);

    DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
                            llvm::DebugLoc::get(LineNo, 0, SP),
                            Builder.GetInsertBlock());
#endif

    // Store the initial value into the alloca.
    Builder.CreateStore(&Arg, Alloca);

    // Add arguments to variable symbol table.
    NamedValues[Arg.getName()] = Alloca;
  }

#ifdef __GENERATE_DWARF
  KSDbgInfo.emitLocation(Body.get());
#endif

  if (llvm::Value *RetVal = Body->codegen()) {
    // Finish off the function
    Builder.CreateRet(RetVal);

#ifdef __GENERATE_DWARF
    // Pop off the lexical block for the function.
    KSDbgInfo.LexicalBlocks.pop_back();
#endif

    // Validate the generated code, checking for consistency.
    llvm::verifyFunction(*TheFunction);

#ifdef __USE_FPM_PASSES
    // Optimize the function.
    TheFPM->run(*TheFunction);
#endif

    return TheFunction;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT driver
//===----------------------------------------------------------------------===//

void InitializeModuleAndPassManager(void) {
  // Open a new module.
  TheModule = std::make_unique<llvm::Module>("Kaleidoscope", TheContext);

#ifdef __USE_JIT
  TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());

#ifdef __USE_FPM_PASSES
  // Create a new pass manager attached to it.
  TheFPM = std::make_unique<llvm::legacy::FunctionPassManager>(TheModule.get());

  // Promote allocas to registers.
  TheFPM->add(llvm::createPromoteMemoryToRegisterPass());
  // Do simple "peephole" optimizations and bit-twiddling optzns.
  TheFPM->add(llvm::createInstructionCombiningPass());
  // Reassociate expressions.
  TheFPM->add(llvm::createReassociatePass());
  // Eliminate Common SubExpressions.
  TheFPM->add(llvm::createGVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  TheFPM->add(llvm::createCFGSimplificationPass());

  TheFPM->doInitialization();
#endif
#endif
}

static void HandleDefinition() {
  if (auto FnAST = ParseDefinition()) {
#ifdef __USE_CLI
    if (auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read function definition: ");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
#ifdef __USE_JIT
      TheJIT->addModule(std::move(TheModule));
      InitializeModuleAndPassManager();
#endif
    }
#else
    if (!FnAST->codegen())
      fprintf(stderr, "Error reading function definition:");
#endif
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleExtern() {
  if (auto ProtoAST = ParseExtern()) {
    if (auto *FnIR = ProtoAST->codegen()) {
#ifdef __USE_CLI
      fprintf(stderr, "Read extern: ");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
#endif
      FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = ParseTopLevelExpr()) {
#ifdef __USE_CLI
    if (auto *FnIR = FnAST->codegen()) {
#ifdef __USE_JIT
      // JIT the module containing the anonymous expression, keeping a handle
      // so we can free it later.
      auto H = TheJIT->addModule(std::move(TheModule));
      InitializeModuleAndPassManager();

      // Search the JIT for the __anon_expr symbol.
      auto ExprSymbol = TheJIT->findSymbol("__anon_expr");
      assert(ExprSymbol && "Function not found");

      // Get the symbol's address and cast it to the right type (takes no
      // arguments, returns a double) so we can call it as a native function.
      double (*FP)() =
          (double (*)())(intptr_t)cantFail(ExprSymbol.getAddress());
      fprintf(stderr, "Evaluated to %f\n", FP());

      // Delete the anonymous expression module from the JIT.
      TheJIT->removeModule(H);
#endif
    }
#else
    if (!FnAST->codegen()) {
      fprintf(stderr, "Error generating code for top level expr");
    }
#endif
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
  while (1) {
#ifdef __USE_CLI
    fprintf(stderr, "ready> ");
#endif
    switch (CurTok) {
    case tok_eof:
      return;
    case ';':
      getNextToken(); // ignore top-level semicolons.
      break;
    case tok_def:
      HandleDefinition();
      break;
    case tok_extern:
      HandleExtern();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef LLVM_ON_WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
  fputc((char)X, stderr);
  return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main() {
#ifdef __USE_JIT
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
#else
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  auto TargetTriple = llvm::sys::getDefaultTargetTriple();
  std::string Error;
  auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);

  // Print an error and exit if we couldn't find the requested target.
  // This generally occurs of we've forgotten to initialise the Target
  // Registry or we have a bogus target triple.
  if (!Target) {
    llvm::errs() << Error;
    return 1;
  }

  auto CPU = "generic";
  auto Features = "";

  llvm::TargetOptions opt;
  auto RM = llvm::Optional<llvm::Reloc::Model>();
  auto TargetMachine =
      Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);
#endif

  // Install standard binary operators.
  // 1 is lowest precedence.
  BinopPrecedence['='] = 2;
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40;

// Prime the first token.
#ifdef __USE_CLI
  fprintf(stderr, "ready> ");
#endif
  getNextToken();

#ifdef __USE_JIT
  TheJIT = std::make_unique<llvm::orc::KaleidoscopeJIT>();
#endif

  InitializeModuleAndPassManager();

#ifdef __GENERATE_DWARF
  DBuilder = std::make_unique<llvm::DIBuilder>(*TheModule);

  KSDbgInfo.TheCU = DBuilder->createCompileUnit(
      llvm::dwarf::DW_LANG_C, DBuilder->createFile("fib.ks", "."),
      "Kaleidoscope Compiler", 0, "", 0);
#endif

#ifndef __USE_JIT
  TheModule->setDataLayout(TargetMachine->createDataLayout());
  TheModule->setTargetTriple(TargetTriple);
#endif

  // Run the main "interpreter loop" now.
  MainLoop();

#ifdef __GENERATE_DWARF
  DBuilder->finalize();
#endif

  // Print out all of the generated code.
  TheModule->print(llvm::errs(), nullptr);

#ifndef __USE_JIT
  // Emit Object code
  auto Filename = "output.o";
  std::error_code EC;
  llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::F_None);

  if (EC) {
    llvm::errs() << "Could not open file: " << EC.message();
    return 1;
  }

  llvm::legacy::PassManager pass;
  auto FileType = llvm::TargetMachine::CGFT_ObjectFile;

  if (TargetMachine->addPassesToEmitFile(pass, dest, FileType)) {
    llvm::errs() << "TargetMachine can't emit a file of this type";
    return 1;
  }

  pass.run(*TheModule);
  dest.flush();

#ifdef __USE_CLI
  llvm::outs() << "Wrote " << Filename << "\n";
#endif
#endif

  return 0;
}
