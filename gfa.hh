#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <iostream>

#define TAG     std::cerr << __FILE__ "@" << __LINE__ << std::endl
#define DMP(x)  std::cerr << #x ": " << (x) << std::endl
#define DMPX(x) std::cerr << #x ": 0x" << std::hex << ((int)(x)) << std::dec << std::endl

// Gallois Field Arithmatic
// uses 2 stages of lookup table to speed up arithmatic.
class GFA
{
public:
    // create an instance of the class to do arithmatic
    GFA()
        // we do this to allow negative indecies in gfilog
        : gfilog(gflog + (2 * two2N))
        {
            uint8_t b = 1;

            // calculate the inverse log of every power of 2
            for (uint8_t l = 0; l < mask; l++)
            {
                // b = 2 ** l, so log(b) = l ...
                gflog[b]  = l;
                // ... and inverse-log(l) = b
                gfilog[l] = b;

                // double b modulo the primitive polynomial
                // (this is the gallois field magic)
                b = (b << 1) ^ ((b & 0x80) ? primPoly : 0);
            }

            // copy the inverse-log table up and down so that
            // gfilog[-mask .. 2*mask] are now valid
            // this speeds up slowMult() and div() below
            memcpy(gfilog - mask, gfilog, mask * sizeof(uint8_t));
            memcpy(gfilog + mask, gfilog, mask * sizeof(uint8_t));

            // finally, create a lookup table for multiplications.
            // multiplications are going to be used a lot, so
            // this should be worth it ...
            for (int a = 0; a < 256 ; a++)
            {
                for (int b = 0; b < 256 ; b++)
                {
                    multLookup[(a << 8) + b] = slowMult(a, b);
                }
            }
        };

    // GF log
    uint8_t log(uint8_t a)
        {
            assert(a);
            return gflog[a];
        };

    // GF inverse log
    uint8_t ilog(uint8_t a)
        {
            return gfilog[a];
        };

    // fast mult, just use the lookup table
    inline uint8_t mult(uint8_t a, uint8_t b)
        {
            return multLookup[(a << 8) + b];
        };

    // slow multiplication
    uint8_t slowMult(uint8_t a, uint8_t b)
        {
            // (0 * 0) == (a * 0) == (0 * b) == 0
            if (!a || !b)
            {
                return 0;
            }
            // a * b = 2 ** (log2(a) + log2(b))
            return gfilog[(gflog[a] + gflog[b])];
        };

    // division is only used when generating the recovery matrix, so no
    // point in creating a lookup table
    uint8_t div(uint8_t a, uint8_t b)
        {
            // (a / 0) = ERROR for all a
            assert(b);
            // (0 / b) = 0 for all b != 0
            if(!a)
            {
                return 0;
            }
            // a / b = exp(log(a) - log(b))
            return gfilog[gflog[a] - gflog[b]];
        };

private:

    // primitive polynomial
    // x**8 + x**4 + x**3 + x**2 + 1
    static const uint8_t primPoly = 0x1d;
    // 2 to the power of N, N = number of bits = 8
    static const uint16_t two2N = 1 << (8 * sizeof(uint8_t));
    // (2**N)-1 . Used often enough to make it worth while
    static const uint8_t mask = two2N - 1;

    // lookup table to speed up multiplication
    uint8_t multLookup[two2N * two2N];
    // lookup tables to speed up multiplication (used to create
    // multLookup) and division
    uint8_t   gflog[4 * two2N];
    uint8_t * gfilog;

private:
    // verify that (c == d), else print a,b,c,d and the message and die
    static void test(uint8_t a,
                     uint8_t b,
                     uint8_t c,
                     uint8_t d,
                     const char * msg)
        {
            if (c == d)
            {
                return;
            }
            std::cerr << msg << "\n\t"
                      << std::hex << (int)a << ", " << (int)b << ", "
                      << std::hex << (int)c << ", " << (int)d
                      << std::endl;
            exit(1);
        };

public:
    std::ostream & operator<<(std::ostream & os)
        {
            os << "log/ilog" << std::endl;
            for (unsigned i = 0; i < 256; ++i)
            {
                os << '\t' << (unsigned)i;
            }
            os << std::endl << "\tX";
            //log(0) ==-inf, so skip that one
            for (unsigned i = 1; i < 256; ++i)
            {
                os << '\t' << (unsigned)gflog[i];
            }
            os << std::endl;
            for (unsigned i = 0; i < 255; ++i)
            {
                os << '\t' << (unsigned)gfilog[i];
            }
            os << "\tX" << std::endl;

            os << "mult" << std::endl;
            for (unsigned i = 1; i < 256; ++i)
            {
                for (unsigned j = 1; j < 256; ++j)
                {
                    os << '\t' << (unsigned)mult(i,j);
                }
                os << std::endl;
            }

            os << "div" << std::endl;
            for (unsigned i = 1; i < 256; ++i)
            {
                for (unsigned j = 1; j < 256; ++j)
                {
                    os << '\t' << (unsigned)div(i,j);
                }
                os << std::endl;
            }

            return os;
        };

    // built-in test
    void BIT()
        {
            // verify that         0 * 0 == 0
            test(0,0,mult(0,0),0, "0 * 0 != 0");
            // cycle through all possible values of a
            for (uint8_t a = 1; a; a++)
            {
                // verify that         0 * a == 0
                test(0,a,mult(0,a),0, "0 * a != 0");
                // verify that         a * 0 == 0
                test(a,0,mult(a,0),0, "a * 0 != 0");
                // unless a is zero ...
                if(a)
                {
                    // verify that a == 2**(log(a))
                    test(a,log(a),a,ilog(log(a)), "ilog(log(a)) != a");
                }
                /*
                  log(0) is undefined.
                  Of the 256 possible numbers only 255 have a valid log/exponent
                  when
                  a            = ff
                  ilog(a)      = af
                  log(ilog(a)) = 0
                */
                if(a != 0xff)
                {
                    // verify that a == log(2**a)
                    test(a,ilog(a),a,log(ilog(a)), "log(ilog(a)) != a");
                }
                // cycle through all possible values of b
                for (uint8_t b = 1; b; b++)
                {
                    uint8_t c = mult(a,b);
                    uint8_t d = mult(b,a);

                    // verify that (a * b) == (b * a)
                    test(a, b, c, d, "a*b != b*a");

                    if(a)
                    {
                        // verify that ((a * b)/a) == b
                        d = div(c,a);
                        test(a, b, d, b, "(a*b)/a != b");
                    }
                }
            }

        };
};

