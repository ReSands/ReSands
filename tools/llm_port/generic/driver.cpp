// Generic judge driver: prints run(<file bytes>). Not written by the model.
#include "task.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string out = run(d.data(), d.size());
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
