#pragma once
// Deterministic bounded expression parser; no eval(), allocation, or UI dependency.
bool calculate(const char* expression,double& result,const char*& error);
