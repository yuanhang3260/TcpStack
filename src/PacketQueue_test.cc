#include "UnitTest/UnitTest.h"
#include "Base/Utils.h"
#include "Base/Log.h"
#include "Base/Ptr.h"

#include "PacketQueue.h"

namespace net_stack {

class PacketQueueTest: public UnitTest {
 public:
  void setup() {
    IPHeader ip_header;
    TCPHeader tcp_header;
    tcp_header.seq_num = 1;
    auto pkt1 = ptr::MakeUnique<Packet>(ip_header, tcp_header);
    tcp_header.seq_num = 2;
    auto pkt2 = ptr::MakeUnique<Packet>(ip_header, tcp_header);
    tcp_header.seq_num = 3;
    auto pkt3 = ptr::MakeUnique<Packet>(ip_header, tcp_header);
    packet_queue_.Push(std::move(pkt1));
    packet_queue_.Push(std::move(pkt2));
    packet_queue_.Push(std::move(pkt3));
  }

  void TestPush() {
    AssertEqual(3, packet_queue_.size());
  }

  void TestDeQueue() {
    auto pkt1 = packet_queue_.DeQueue();
    AssertEqual(2, packet_queue_.size());
    AssertEqual(1, pkt1->tcp_header().seq_num);
  }

  void TestDeQueueAll() {
    std::queue< std::unique_ptr<Packet> > receiver_queue_;
    AssertEqual(2, packet_queue_.DeQueueAllTo(&receiver_queue_));
    AssertEqual(2, receiver_queue_.front()->tcp_header().seq_num);
    receiver_queue_.pop();
    AssertEqual(3, receiver_queue_.front()->tcp_header().seq_num);
  }
  
 protected:
  PacketQueue packet_queue_;
};

}  // namespace net_stack

int main() {
  net_stack::PacketQueueTest test;
  test.setup();
  test.TestPush();
  test.TestDeQueue();
  test.TestDeQueueAll();
  test.teardown();

  std::cout << "\033[2;32mPassed ^_^\033[0m" << std::endl;
  return 0;
}
