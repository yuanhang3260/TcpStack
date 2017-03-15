#include "UnitTest/UnitTest.h"
#include "Base/Utils.h"
#include "Base/Log.h"
#include "Base/Ptr.h"

#include "SendWindow.h"

namespace net_stack {

class SendWindowTest: public UnitTest {
 public:
  SendWindowTest() : send_window_(0, 16) {}

  void setup() {
  }

  void TestSend() {
    AssertTrue(send_window_.SendPacket(MakePacket(0, "hello")));
    AssertTrue(send_window_.SendPacket(MakePacket(5, "hello")));
    AssertTrue(send_window_.SendPacket(MakePacket(10, "hello")));
    AssertFalse(send_window_.SendPacket(MakePacket(15, "hello")));
  }

  void TestAckInOrder() {
    AssertTrue(send_window_.SendPacket(MakePacket(0, "hello")));
    AssertTrue(send_window_.SendPacket(MakePacket(5, "hello")));
    AssertTrue(send_window_.SendPacket(MakePacket(10, "hello")));

    AssertTrue(send_window_.NewAckedPacket(5));
    AssertEqual(5, send_window_.send_base());

    AssertTrue(send_window_.NewAckedPacket(10));
    AssertEqual(10, send_window_.send_base());

    AssertTrue(send_window_.NewAckedPacket(15));
    AssertEqual(15, send_window_.send_base());
  }

  void TestAccumulativeAck() {
    AssertTrue(send_window_.SendPacket(MakePacket(0, "hello")));
    AssertTrue(send_window_.SendPacket(MakePacket(5, "hello")));
    AssertTrue(send_window_.SendPacket(MakePacket(10, "hello")));

    AssertTrue(send_window_.NewAckedPacket(5));
    AssertEqual(5, send_window_.send_base());

    AssertTrue(send_window_.NewAckedPacket(15));
    AssertEqual(15, send_window_.send_base());
  }

  void TestDuplicatedAck() {
    AssertTrue(send_window_.SendPacket(MakePacket(0, "hello")));
    AssertTrue(send_window_.SendPacket(MakePacket(5, "hello")));
    AssertTrue(send_window_.SendPacket(MakePacket(10, "hello")));

    AssertTrue(send_window_.NewAckedPacket(5));
    AssertEqual(5, send_window_.send_base());

    AssertTrue(send_window_.NewAckedPacket(5));
    AssertEqual(5, send_window_.send_base());

    // Third ack num 5. Maybe we should re-transmit packet 5.
    AssertFalse(send_window_.NewAckedPacket(5));
    AssertEqual(5, send_window_.send_base());

    AssertTrue(send_window_.NewAckedPacket(15));
    AssertEqual(15, send_window_.send_base());
  }

  void TestAckOverFlow() {
    uint32 max_uint32 = ~(uint32)0x1 + 1;
    SendWindow send_window(max_uint32 - 6, 25);
    AssertTrue(send_window.SendPacket(MakePacket(max_uint32 - 6, "hello")));
    AssertTrue(send_window.SendPacket(MakePacket(max_uint32 - 1, "hello")));
    AssertTrue(send_window.SendPacket(MakePacket(3, "hello")));
    AssertTrue(send_window.SendPacket(MakePacket(8, "hello")));
    AssertTrue(send_window.SendPacket(MakePacket(13, "hello")));
    AssertEqual(5, send_window.NumPacketsToAck());

    AssertTrue(send_window.NewAckedPacket(13));
    AssertEqual((uint32)13, send_window.send_base());
    AssertEqual((uint32)5, send_window.size());
    AssertEqual(1, send_window.NumPacketsToAck());

    AssertTrue(send_window.NewAckedPacket(18));
    AssertEqual((uint32)18, send_window.send_base());
    AssertEqual((uint32)0, send_window.size());
    AssertEqual(0, send_window.NumPacketsToAck());
  }

 protected:
  std::shared_ptr<Packet> MakePacket(uint32 tcp_seq_num,
                                     const std::string& data) {
    IPHeader ip_header;
    TcpHeader tcp_header;
    tcp_header.seq_num = tcp_seq_num;
    std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header, data));
    return pkt;
  }

  SendWindow send_window_;
};

}  // namespace net_stack

int main() {
  net_stack::SendWindowTest test;
  test.setup();
  //test.TestSend();
  //test.TestAckInOrder();
  //test.TestAccumulativeAck();
  //test.TestDuplicatedAck();
  test.TestAckOverFlow();
  test.teardown();

  std::cout << "\033[2;32mPassed ^_^\033[0m" << std::endl;
  return 0;
}
