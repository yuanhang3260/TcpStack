#include "Base/Log.h"
#include "Base/Utils.h"
#include "BaseChannel.h"
#include "Host.h"
#include "TcpController.h"
#include "Utility/ThreadPool.h"

using Executors::FixedThreadPool;
using net_stack::BaseChannel;
using net_stack::Host;
using std::placeholders::_1;

namespace {
const char* const kAliceIP = "127.0.0.1";
const char* const kBobIP = "127.0.0.2";
const uint32 kAlicePort = 10;
const uint32 kBobPort = 20;
const uint32 kAliceSocket = 5;
const uint32 kBobSocket = 5;

const uint32 kTestDataSize = 20;
byte* data;
byte* receive_buffer;

void InitData() {
  data = new byte[kTestDataSize];
  for (uint32 i = 0; i < kTestDataSize; i++) {
    data[i] = Utils::RandomNumber(256);
  }

  receive_buffer = new byte[kTestDataSize];
}

bool ReceivedDataCorrect() {
  for (uint32 i = 0; i < kTestDataSize; i++) {
    if (receive_buffer[i] != data[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace shutdown

int main(int argc, char** argv) {
  // Channel is unidirectional. Define two channels.
  BaseChannel channel_alice_to_bob;
  BaseChannel channel_bob_to_alice;

  // Hello, Alice and Bob.
  Host alice(kAliceIP, &channel_alice_to_bob);
  Host bob(kBobIP, &channel_bob_to_alice);

  // Register receiver's callback to channel.
  channel_alice_to_bob.RegisterReceiverCallback(
      std::bind(&Host::MovePacketsFromChannel, &bob, _1));
  channel_bob_to_alice.RegisterReceiverCallback(
      std::bind(&Host::MovePacketsFromChannel, &alice, _1));

  channel_alice_to_bob.Start();
  channel_bob_to_alice.Start();

  alice.CreateTcpConnection(kBobIP, kBobPort, kAlicePort, kAliceSocket);
  bob.CreateTcpConnection(kAliceIP, kAlicePort, kBobPort, kBobSocket);

  // Create data to send.
  InitData();

  auto alice_thread = [&] () {
    uint32 writen = 0;
    while (writen < kTestDataSize) {
      auto re = alice.WriteData(kAliceSocket, data + writen, kTestDataSize);
      if (re > 0) {
        writen += re;
      }
    }
    if (writen != kTestDataSize) {
      LogERROR("Alice sent %d bytes data\n", writen);
    }
  };

  auto bob_thread = [&] () {
    uint32 readn = 0;
    while (readn < kTestDataSize) {
      auto re = bob.ReadData(kBobSocket, receive_buffer + readn, kTestDataSize);
      if (re > 0) {
        readn += re;
      }
    }
    if (readn != kTestDataSize) {
      LogERROR("Bob received %d bytes data\n", readn);
      return;
    }
    if (!ReceivedDataCorrect()) {
      LogERROR("Receive data failed");
    } else {
      printf("Bob received correct data :)\n");
    }
  };

  FixedThreadPool thread_pool(2);
  thread_pool.AddTask(alice_thread);
  thread_pool.AddTask(bob_thread);
  thread_pool.Start();

  while (true) {}
}
