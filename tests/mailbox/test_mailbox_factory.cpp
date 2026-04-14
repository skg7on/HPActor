#include <cassert>
#include <hpactor/core/mailbox.hpp>
#include <hpactor/actor/message.hpp>
#include <hpactor/core/mutex_mailbox.hpp>

struct SimpleMsg {};

int main() {
    auto mailbox =
        hpactor::create_mailbox<SimpleMsg, hpactor::MailboxType::Mutex>();
    mailbox->push(hpactor::Message<SimpleMsg>{});
    assert(mailbox->size() == 1);
    return 0;
}