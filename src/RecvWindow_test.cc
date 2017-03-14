#include "UnitTest/UnitTest.h"
#include "Base/Utils.h"
#include "Base/Log.h"
#include "Base/Ptr.h"

#include "RecvWindow.h"

namespace net_stack {

class RecvWindowTest: public UnitTest {
 public:
  RecvWindowTest() : recv_window_(0, 65536) {}

  void setup() {
  }

  void TestReceiveOrderly() {
    for (int i = 0; i < 100; i++) {
      auto result = recv_window_.ReceivePacket(MakePacket(i * 5, "hello"));
      AssertEqual((i + 1) * 5, result.first);
      AssertEqual(1, PacketChainSize(result.second));
    }
  }
  
 protected:
  std::unique_ptr<Packet> MakePacket(uint32 tcp_seq_num,
                                     const std::string& data) {
    IPHeader ip_header;
    TcpHeader tcp_header;
    tcp_header.seq_num = tcp_seq_num;
    auto pkt = ptr::MakeUnique<Packet>(ip_header, tcp_header, data);
    return pkt;
  }

  uint32 PacketChainSize(std::shared_ptr<RecvWindow::RecvWindowNode> node) {
    uint32 size = 0;
    while (node) {
      size++;
      node = node->next;
    }
    return size;
  }

  RecvWindow recv_window_;
};

}  // namespace net_stack

int main() {
  net_stack::RecvWindowTest test;
  test.setup();
  test.TestReceiveOrderly();
  test.teardown();

  std::cout << "\033[2;32mPassed ^_^\033[0m" << std::endl;
  return 0;
}
