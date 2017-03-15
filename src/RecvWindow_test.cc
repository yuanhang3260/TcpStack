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

  void TestReceiveOutOfOrder() {
    auto result = recv_window_.ReceivePacket(MakePacket(0, "hello"));
    AssertEqual(5, result.first);
    AssertEqual(1, PacketChainSize(result.second));

    result = recv_window_.ReceivePacket(MakePacket(10, "hello"));
    AssertEqual(5, result.first);
    AssertEqual(0, PacketChainSize(result.second));

    result = recv_window_.ReceivePacket(MakePacket(15, "hello"));
    AssertEqual(5, result.first);
    AssertEqual(0, PacketChainSize(result.second));

    result = recv_window_.ReceivePacket(MakePacket(25, "hello"));
    AssertEqual(5, result.first);
    AssertEqual(0, PacketChainSize(result.second));

    result = recv_window_.ReceivePacket(MakePacket(5, "hello"));
    AssertEqual(20, result.first);
    AssertEqual(3, PacketChainSize(result.second));

    result = recv_window_.ReceivePacket(MakePacket(30, "hello"));
    AssertEqual(20, result.first);
    AssertEqual(0, PacketChainSize(result.second));

    result = recv_window_.ReceivePacket(MakePacket(20, "hello"));
    AssertEqual(35, result.first);
    AssertEqual(3, PacketChainSize(result.second));
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
    if (!node) {
      return 0;
    }

    AssertTrue(!node->prev);
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
  // test.TestReceiveOrderly();
  test.TestReceiveOutOfOrder();
  test.teardown();

  std::cout << "\033[2;32mPassed ^_^\033[0m" << std::endl;
  return 0;
}
