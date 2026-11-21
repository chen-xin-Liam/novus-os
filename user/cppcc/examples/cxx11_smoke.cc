#include <novus/syscall.h>

enum class Color : int { Red = 1, Blue = 2 };
using Index = int;
static_assert(sizeof(Index) == 4, "Index must be 32-bit");

int main() {
    int values[2] = {1, 2};
    Index total = 0;
    for (Index value : values) {
        total += value;
    }
    Color chosen = Color::Blue;
    if (total == 3 && chosen == Color::Blue) {
        novus_write("ok", 2);
        return 0;
    }
    return 1;
}
