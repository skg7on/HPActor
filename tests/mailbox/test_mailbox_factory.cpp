#include <cassert>
#include <hpactor/mailbox.hpp>
#include <hpactor/message.hpp>
#include <hpactor/mutex_mailbox.hpp>

struct SimpleMsg {};

int main() {
    auto mailbox =
        hpactor::create_mailbox<SimpleMsg, hpactor::MailboxType::Mutex>();
    mailbox->push(hpactor::Message<SimpleMsg>{});
    assert(mailbox->size() == 1);
    return 0;
}