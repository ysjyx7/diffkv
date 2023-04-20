#include "util.h"
#include <iostream>

#include "test_util/testharness.h"

namespace rocksdb {
namespace titandb {
std::string func(){
   return "Hello";
}
class HelloTest : public testing::Test {};


TEST(HelloTest, hello) {
    ASSERT_EQ("Hello", func());
}

}  // namespace titandb
}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

