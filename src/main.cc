#include "BaseChannel.h"
#include "Host.h"
#include "TcpController.h"

using net_stack:BaseChannel;
using net_stack:Host;

int main(int argc, char** argv) {
  // Channel is unidirectional. Define two channels.
  BaseChannel channel_alice_to_bob;
  BaseChannel channel_bob_to_alice;

  // Hello, Alice and Bob.
  Host alice("127.0.0.1", &channel_alice_to_bob);
  Host bob("127.0.0.2", &channel_bob_to_alice);

  // Register receiver's callback to channel.
  channel_alice_to_bob.RegisterReceiverCallback(
      std::bind(&Host::MovePacketsFromChannel, &bob));
  channel_bob_to_alice.RegisterReceiverCallback(
      std::bind(&Host::MovePacketsFromChannel, &alice));

  channel_bob_to_alice.Start();
  channel_bob_to_alice.Start();
}