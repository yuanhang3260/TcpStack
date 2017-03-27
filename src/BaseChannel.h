#ifndef NET_STACK_BASE_CHANNEL_
#define NET_STACK_BASE_CHANNEL_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "Base/BaseTypes.h"
#include "PacketQueue.h"

namespace net_stack {

// This is an abstraction of network channel with unlimited volume. Note this is
// a unidirection channel. Basically senders call Send to push new packet into
// the channel and receiver on the other side will be notified to receive.
// It can simulate data corruption, packet loss and transmit delay to behave as
// an unreliable channel.
//
// This class is thread-safe, meaning it supports multiple senders sending
// packets, and multiple receivers. But in actual usage, there should be only
// one receiver which registers a receiving callback function. It'll be called
// by channel whenever there is packet available. The callback should be fast
// to avoid packet accumulating inside channel. Typically it should just move
// packets to receiver's own buffer.
class BaseChannel {
 public:
  using ReceiverCallBack =
      std::function<void(std::queue<std::unique_ptr<Packet>>*)>;

  BaseChannel() = default;
  explicit BaseChannel(ReceiverCallBack receiver_callback);
  ~BaseChannel();

  void Start();

  // Unfortunately the name is a bit confusing. It's sending packet to channel,
  // rather than channel sending packets out to receiver, if you see code like
  // "channel.Send(pkt)".
  void Send(std::unique_ptr<Packet> packet);
  void Send(std::queue<std::unique_ptr<Packet>>* pkts);
  // It takes ownership of the packet.
  void Send(Packet* packet);

  void RegisterReceiverCallback(ReceiverCallBack receiver_callback);

 private:
  // This is a listening thread which monitors channel buffer, and notify
  // receiver when new packets are available.
  void WaitingForPackets();

  std::mutex pkt_buffer_mutex_;
  std::condition_variable pkt_buffer_cv_;
  PacketQueue pkt_buffer_;

  std::mutex cb_mutex_;
  ReceiverCallBack receiver_callback_;

  std::thread listner_;
  bool stop_ = true;
};

}  // namespace net_stack

#endif  // NET_STACK_BASE_CHANNEL_