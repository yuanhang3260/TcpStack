#include <unistd.h>

#include "UnitTest/UnitTest.h"
#include "Base/Utils.h"
#include "Base/Log.h"
#include "Base/Ptr.h"
#include "Utility/ThreadPool.h"

#include "BaseChannel.h"

namespace net_stack {

class BaseChannelTest: public UnitTest {
 public:
  void setup() {
    channel_.RegisterReceiverCallback(
      [&] (std::queue<std::unique_ptr<Packet>>& new_packets) {
        //printf("Receiving packets\n");
        num_packets_received_ += new_packets.size();
        new_packets.swap(receiver_buffer_);
      });
    channel_.Start();
  }

  std::unique_ptr<Packet> MakePacket(int tcp_seq_num) {
    IPHeader ip_header;
    TcpHeader tcp_header;
    tcp_header.seq_num = tcp_seq_num;
    auto pkt = ptr::MakeUnique<Packet>(ip_header, tcp_header);
    return pkt;
  }

  void test_Send() {
    channel_.Send(MakePacket(1));
    channel_.Send(MakePacket(2));
    channel_.Send(MakePacket(3));
    usleep(1 * 100 * 1000);  // 0.1s is enough?
    AssertEqual(3, receiver_buffer_.size());
    AssertEqual(3, num_packets_received_);

    channel_.Send(MakePacket(4));
    channel_.Send(MakePacket(5));
    usleep(1 * 100 * 1000);
    AssertEqual(2, receiver_buffer_.size());
    AssertEqual(5, num_packets_received_);
  }

  void test_LoadTest() {
    // 1000 threads sending 1000 *30 packets.
    num_packets_received_ = 0;
    Executors::FixedThreadPool thread_pool(100);
    for (int i = 0; i < 1000; i++) {
      thread_pool.AddTask([&] {
        for (int j = 0; j < 30; j++) {
          channel_.Send(MakePacket(j));
        }
      });
    }
    thread_pool.Start();
    usleep(5 * 100 * 1000);  // 0.5s

    thread_pool.Stop();
    thread_pool.AwaitTermination();
    AssertEqual(30 * 1000, num_packets_received_);
  }

 protected:
  BaseChannel channel_;
  std::queue<std::unique_ptr<Packet>> receiver_buffer_;
  int num_packets_received_ = 0;
};

}  // namespace net_stack

int main() {
  net_stack::BaseChannelTest test;
  test.setup();
  //test.test_Send();
  test.test_LoadTest();
  test.teardown();

  std::cout << "\033[2;32mPassed ^_^\033[0m" << std::endl;
  return 0;
}
