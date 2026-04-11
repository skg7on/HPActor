#include <cassert>
#include <hpactor/mailbox.hpp>
#include <hpactor/message.hpp>

using namespace hpactor;

int main() {
    hpactor::ActorMailbox<int> mailbox;

    // Test empty on construction
    assert(mailbox.empty());
    assert(mailbox.size() == 0);

    return 0;
}