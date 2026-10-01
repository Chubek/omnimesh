#include "omnimesh/status.hpp"
#include <iostream>
#include <string>
int main(int argc,char** argv){if(argc>1&&std::string(argv[1])=="--version"){std::cout<<omnimesh::version()<<"\n";return 0;}std::cout<<"omnimesh scaffold: validation and execution are not implemented\n";return 0;}
