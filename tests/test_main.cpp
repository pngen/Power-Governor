#include "test_framework.hpp"
int main(int argc, char** argv) {
  const char* filter = (argc > 1) ? argv[1] : nullptr;
  return pgtest::run_all(argc, argv, filter);
}
