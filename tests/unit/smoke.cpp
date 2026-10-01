#include "omnimesh/status.hpp"
#include <cassert>
int main(){ assert(omnimesh::Status::NotImplemented("x").code==omnimesh::StatusCode::not_implemented); assert(omnimesh::version()[0]=='0'); return 0; }
