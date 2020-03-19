#include "Halide.h"
#include <stdio.h>

using namespace Halide;
using namespace Halide::Internal;

int main(int argc, char **argv) {
    // select_by_id through variadic template
    {
        Var x("x"), c("c");
        Func f("f");

        f(x, c) = select_by_id(c, x, 456, 789);

        Buffer<int> result = f.realize(100, 4);
        for (int x = 0; x < result.width(); x++) {
            if (result(x, 0) != x) {
                printf("result(%d, 0) = %d instead of %d\n",
                       x, result(x, 0), x);
                return -1;
            }
            if (result(x, 1) != 456) {
                printf("result(%d, 1) = %d instead of %d\n",
                       x, result(x, 1), 456);
                return -1;
            }
            if (result(x, 2) != 789) {
                printf("result(%d, 2) = %d instead of %d\n",
                       x, result(x, 2), 789);
                return -1;
            }
            if (result(x, 3) != 789) {
                printf("result(%d, 3) = %d instead of %d\n",
                       x, result(x, 3), 789);
                return -1;
            }
        }
    }

    // select_by_id through initializer list (std::vector)
    {
        Var x("x"), c("c");
        Func f("f");

        f(x, c) = select_by_id(c, {x, 456, 789});

        Buffer<int> result = f.realize(100, 4);
        for (int x = 0; x < result.width(); x++) {
            if (result(x, 0) != x) {
                printf("result(%d, 0) = %d instead of %d\n",
                       x, result(x, 0), x);
                return -1;
            }
            if (result(x, 1) != 456) {
                printf("result(%d, 1) = %d instead of %d\n",
                       x, result(x, 1), 456);
                return -1;
            }
            if (result(x, 2) != 789) {
                printf("result(%d, 2) = %d instead of %d\n",
                       x, result(x, 2), 789);
                return -1;
            }
            if (result(x, 3) != 789) {
                printf("result(%d, 3) = %d instead of %d\n",
                       x, result(x, 3), 789);
                return -1;
            }
        }
    }

    printf("Success!\n");
    return 0;
}
