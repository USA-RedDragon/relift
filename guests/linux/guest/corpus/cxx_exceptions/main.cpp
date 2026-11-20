#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

struct Resource {
    const char *name;
    explicit Resource(const char *n) : name(n) { std::printf("acquire %s\n", name); }
    ~Resource() { std::printf("release %s\n", name); }
};

struct Custom {
    int code;
};

static int depth(int n)
{
    Resource r("frame");
    if (n == 0)
        throw Custom{42};
    return depth(n - 1) + 1;
}

static void rethrower()
{
    try {
        throw std::runtime_error("inner failure");
    } catch (const std::exception &e) {
        std::printf("caught %s, rethrowing\n", e.what());
        throw;
    }
}

int main()
{
    try {
        depth(2);
    } catch (const Custom &c) {
        std::printf("custom code %d\n", c.code);
    }
    try {
        rethrower();
    } catch (const std::runtime_error &e) {
        std::printf("outer got %s\n", e.what());
    }
    try {
        std::vector<int> v(3);
        (void)v.at(7);
    } catch (const std::out_of_range &) {
        std::printf("out_of_range\n");
    }
    try {
        throw 17;
    } catch (const char *) {
        std::printf("wrong handler\n");
    } catch (int i) {
        std::printf("int %d\n", i);
    }
    std::string s;
    for (int i = 0; i < 5; i++)
        s += std::to_string(i * i) + ",";
    std::printf("%s\n", s.c_str());
    return 0;
}
