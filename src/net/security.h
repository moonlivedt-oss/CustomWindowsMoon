#pragma once

#include <string>

namespace net {

enum SecurityRisk : unsigned { FIREWALL_OFF = 1, WIFI_OPEN = 2, WIFI_LEGACY = 4 };
struct SecurityState {
    unsigned known = 0, risks = 0;  // неизвестное состояние не считается ни угрозой, ни исправлением
};

SecurityState ReadSecurity();  // только локальные API, без сканирования Wi-Fi и чтения паролей
std::wstring SecurityAdvice(unsigned risks);
bool BackgroundTrafficAllowed();  // только явно безлимитная сеть; ошибка/неизвестно — отложить большой замер

// Два последовательных подтверждения перед первой карточкой. Неизвестный ответ не снимает уже
// подтверждённый риск; пока он сохраняется, повторные карточки не приходят.
class SecurityLatch {
public:
    unsigned Observe(SecurityState s) {
        unsigned active = s.risks & s.known;
        unsigned added = active & previous_ & ~confirmed_;
        confirmed_ = (confirmed_ & ~s.known) | (active & (previous_ | confirmed_));
        previous_ = active;
        return added;
    }
    unsigned Risks() const { return confirmed_; }

private:
    unsigned previous_ = 0, confirmed_ = 0;
};

}  // namespace net
