#include "eventloop.hpp"
#include <iostream>

using namespace std;

void print(const vector<int>& nums) {
    printf("nums: ");
    for (size_t i = 0; i < nums.size(); i++)
        printf("%d ", nums[i]);
    
    printf("\n");
}

void tst(int a, int b)
{
    LOG("a: %d, b: %d\n", a, b);
}
void tst2()
{
    LOG("no args\n");
}

int main(int argc, char** argv) {

    EventLoop loop;
    loop.loop();

    auto t0 = loop.runEvery(100, true, []() {
        LOG("test every 100\n");
    });

    auto t1 = loop.runEvery(200, true, []() {
        LOG("test every 200\n");
    });

    auto t2 = loop.runEvery(1000, true, []() {
        LOG("test every 1000\n");
    });

    auto t3 = loop.runAfter(1200, []() {
        LOG("test after 1000\n");
    });
    loop.cancelTimer(t3);

    loop.runInLoop([]() {
        LOG("test in loop\n");
    });

    loop.runInLoop(tst2);
    loop.runInLoop(tst, 1, 2);

    auto t4 = loop.runEvery(1000, true, tst, 1, 2);

    sleep(2);
    loop.cancelTimer(t0);
    loop.cancelTimer(t1);

    std::string s;
    while (1) 
    {
        std::cin >> s;
        if (s == "q") 
        {
            loop.quit();
            break;
        }
        else if(s == "r")
            loop.runInLoop([](){ LOG("in loop");});
    }

    while (1) sleep(1);

    return 0;
}
