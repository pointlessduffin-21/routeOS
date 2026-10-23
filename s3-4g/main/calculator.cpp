#include "calculator.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
namespace {
class Parser {
  const char* p;
  unsigned depth=0;
  void space(){while(*p==' ')++p;}
  double atom() {
    space();
    if(++depth>16){error="Too deeply nested";--depth;return 0;}
    double value=0;
    if(*p=='(') {++p;value=sum();space();if(*p!=')')error="Missing )";else ++p;}
    else {char* end;value=strtod(p,&end);if(end==p)error="Finish expression";else p=end;}
    space();
    while(*p=='%'){value/=100.0;++p;space();}
    --depth;return value;
  }
  double power() {
    double value=atom();space();
    if(*p=='^' && !error) {++p;if(++depth>16){error="Too deeply nested";return 0;}value=pow(value,unary());--depth;}
    return value;
  }
  double unary() {
    space();
    if(*p=='+' || *p=='-') {
      if(++depth>16){error="Too deeply nested";--depth;return 0;}
      char sign=*p++;double value=unary();--depth;return sign=='-'?-value:value;
    }
    return power();
  }
  double product() {
    double value=unary();space();
    while((*p=='*' || *p=='/') && !error) {
      char op=*p++;double rhs=unary();
      if(op=='/' && rhs==0){error="Divide by zero";return 0;}
      value=op=='*'?value*rhs:value/rhs;space();
    }
    return value;
  }
public:
  const char* error=nullptr;
  explicit Parser(const char* s):p(s){}
  double sum() {
    double value=product();space();
    while((*p=='+' || *p=='-') && !error) {char op=*p++;double rhs=product();value=op=='+'?value+rhs:value-rhs;space();}
    return value;
  }
  bool run(double& result) {
    result=sum();space();
    if(!error && *p)error="Invalid expression";
    if(!error && !isfinite(result))error="Out of range";
    return !error;
  }
};
}
bool calculate(const char* expression,double& result,const char*& error) {
  if(!expression || strlen(expression)>64){error="Expression too long";return false;}
  if(!*expression){result=0;error=nullptr;return true;}
  // strtod also accepts NaN/Inf and hex notation; the HMI deliberately does not.
  for(const char* p=expression;*p;++p)if(!strchr("0123456789.eE +-*/()%^",*p)){error="Invalid character";return false;}
  Parser parser(expression);bool ok=parser.run(result);error=parser.error;return ok;
}
