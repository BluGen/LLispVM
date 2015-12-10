#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if __cpluplus < 201402L
namespace helper {
// Cloning make_unique here until it's standard in C++14.
// Using a namespace to avoid conflicting with MSVC's std::make_unique (which
// ADL can sometimes find in unqualified calls).
template <class T, class... Args>
static
    typename std::enable_if<!std::is_array<T>::value, std::unique_ptr<T>>::type
    make_unique(Args &&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
#endif

// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
enum Token {
	tok_eof = -1,

	//	parentheses
	tok_bracket_open = -2,
	tok_bracket_close = -3,

	// primary
	tok_identifier = -5,
	tok_number = -6
};

// Helper class to identify ast element at runtime.
enum class ExprType {
	Number,
	Identifier,
	List
};

const std::string definitionKeyword("set");
static std::unique_ptr<Module> *TheModule;
static llvm::IRBuilder<> Builder(llvm::getGlobalContext());
static std::map<std::string, Value*> NamedValues;

// Helper function to return compile errors
llvm::Value *ErrorV(const char *Str) {
	Error(Str);
	return nullptr;
}

//	Helper function to return parse errors
std::unique_ptr<ExprAST> Error(const char *Str) {
	fprintf(stderr, "Error: %s\n", Str);
	return nullptr;
}

static std::string IdentifierStr;	// Filled in if tok_identifier
static int NumVal;					// Filled in if tok_number

/// gettok - Return the next token from standard input.
static int gettok() {
	static int LastChar = ' ';

	// Skip any whitespace.
	while(isspace(LastChar))
		LastChar = getchar();

	if(LastChar == '('){	// parentheses: [(]
		IdentifierStr += LastChar;
		return tok_bracket_open;
	}
	if(LastChar == ')'){	// parentheses: [)]
		IdentifierStr += LastChar;
		return tok_bracket_close;
	}

	if(isalpha(LastChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
		IdentifierStr = LastChar;
		while(isalnum((LastChar = getchar()))){
			IdentifierStr += LastChar;
		}
		return tok_identifier;
	}

	if(isdigit(LastChar)) { // Number: [1-9][0-9]* | [0]
		std::string NumStr;
	
		if(LastChar == '0'){	// Number: [0]
			NumStr = LastChar; 
		} else {				// Number [1-9][0-9]*
			do {
			  NumStr += LastChar;
			  LastChar = getchar();
			} while (isdigit(LastChar));
		}

		NumVal = strtod(NumStr.c_str(), nullptr);
		return tok_number;
	}

	// Check for end of file.  Don't eat the EOF.
	if (LastChar == EOF)
		return tok_eof;

	// Otherwise, just return the character as its ascii value.
	int ThisChar = LastChar;
	LastChar = getchar();
	return ThisChar;
}

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//
namespace {
/// ExprAST - Base class for all expression nodes.
class ExprAST {
	ExprType type;
public:
	virtual ~ExprAST() {}
	ExprAST(ExprType Type) : type(Type) {};
	ExprType getType() {return type};
	virtual llvm::Value *codegen() = 0;
};

/// NumberExprAST - Expression class for numeric literals like "1".
class NumberExprAST : public ExprAST {
	int Val;

public:
	NumberExprAST(int Val) : ExprAST(ExprType::Number), Val(Val) {}
	virtual llvm::Value *codegen(){
		using namespace llvm;
		/// Create globals int variable
		return ConstantInt::get(getGlobalContext(), APInt(sizeof(int)*8, Val));
	};
};

/// IdentifierExprAST - Expression class for referencing a variable, like "a".
class IdentifierExprAST : public ExprAST {
	std::string Name;

public:
	VariableExprAST(const std::string &Name) : ExprAST(ExprType::Identifier), Name(Name) {}
	virtual llvm::Value *codegen(){
		llvm::Value *V = NamedValues[Name];
		if (!V)
			ErrorV("Unknown variable name");
		return V;
	};
};

///	ListExprAST - Expression class for referencing a list containing further ExprAST objects
class ListExprAST : public ExprAST {
	std::vector<std::unique_ptr<ExprAST>> Items;
	
public:
	ListExprAST(std::vector<std::unique_ptr<ExprAST>> &Items) : ExprAST(ExprType::List), Items(Items){}
	virtual llvm::Value *codegen(){
		
	};
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

/// CurTok/getNextToken - Provide a simple token buffer.  CurTok is the current
/// token the parser is looking at.  getNextToken reads another token from the
/// lexer and updates CurTok with its results.
static int CurTok;
static int getNextToken() { return CurTok = gettok(); }

static std::unique_ptr<ExprAST> ParseNumberExpr() {
	auto Result = helper::make_unique<NumberExprAST>(NumVal);
	getNextToken(); // consume the number
	return std::move(Result);
}

static std::unique_ptr<ExprAST> ParseListExpr() {
	getNextToken(); // eat (
	std::vector<std::unique_ptr<ExprAST>> Items;
	while(1){
		if(auto V = ParseExpr()){
			Items.push_back(std::move(V);
		} else {
			return nullptr;
		}
		
		//	Check if next token is end of list
		if (CurTok == tok_bracket_close){
			break;
		}
		getNextToken();
	}

	getNextToken(); // eat )
	
	return std::move(Items);
}

static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
	std::string& IdName = IdentifierStr;
	return helper::make_unique<VariableExprAST>(IdName);
}

static std::unique_ptr<ExprAST> ParseExpr() {
	switch (CurTok) {
		default:
			return Error("unknown token when expecting an expression");
		case tok_identifier:
			return ParseIdentifierExpr();
		case tok_number:
			return ParseNumberExpr();
		case tok_bracket_open:
			return ParseListExpr();
	}
}

// Initial processing loop
int main(void) {
	// Load the first token.
	fprintf(stderr, "ready> ");
	getNextToken();

	// Run the main "interpreter loop" now.
	while(true){
		fprintf(stderr, "ready> ");
		switch (CurTok) {
			case tok_eof:
				return 0;
			case tok_bracket_open:
				ParseListExpr();
			default:
				fprintf(stderr, "ERROR\tCharacter can not be processed: %c", CurTok);
				return -2;
		}
	}
	// This point should not be reached
	return -1;
}
