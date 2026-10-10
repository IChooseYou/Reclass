#include <windows.h>
#include <cstdio>

struct AutoFixture {
#ifdef WRONG_SCHEMA
    double health;
#else
    int health;
#endif
    unsigned int team;
    float position;
    double weight;
    unsigned char flag;
};
AutoFixture gAutoFixture{42, 7, 1.25f, 19.75, 1};
AutoFixture* gAutoPointer = &gAutoFixture;

int main() {
    std::printf("{\"pid\":%lu,\"address\":\"0x%llx\",\"pointer\":\"0x%llx\",\"size\":%zu}\n",
        GetCurrentProcessId(), (unsigned long long)&gAutoFixture, (unsigned long long)&gAutoPointer,
        sizeof(gAutoFixture));
    std::fflush(stdout);
    for (;;) Sleep(100);
}
