#include "gfa.hh"
#include "git.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/evp.h>
#include <sstream>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>

extern const char _binary_gfm_tar_start;
extern const char _binary_gfm_tar_end;

std::ofstream dumpFile;

size_t blobSize();
size_t  _binary_gfm_tar_len = blobSize();

/// needs to be the same for parity gerneration and recovery.
/// Choose multiples of 512 'cos that's one disk sector.
/// larger values _might_ make it go faster
/// but might waste more on partial blocks
const uint8_t BLOCKSIZE_Po2 = 12;
const size_t  BLOCKSIZE     = 1 << BLOCKSIZE_Po2;

// Signature prepended to data and parity files.
typedef struct _signature
{
    uint8_t numData;
    uint8_t numParity;
    uint8_t fileNum;
    uint8_t blocksizePo2;
} signature;

// fancy assert
void attest(bool test, const char * epilogue = "oops", ...)
{
    if (test)
    {
        return;
    }
    va_list ap;
    va_start(ap, epilogue);
    vfprintf(stderr, epilogue, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

// extract un-padded file size from v7-format tarball
size_t blobSize()
{
    size_t rawSize =
        &_binary_gfm_tar_end -
        &_binary_gfm_tar_start;

    char * endptr = 0;
    uint32_t s = strtol(
        (&_binary_gfm_tar_start) + 124,
        &endptr, 8);
    attest(endptr && (*endptr == '\0'),
           "unable to decode file size from tar header");
    s += 0x200;
    attest((s < rawSize),
           "unable to sensibly decode file size from tar header");
    return s;
}

/// Gallois Field Matrix
class GFM
{
public:
    GFM(uint8_t _numData, uint8_t _numParity)
        : numData(_numData)
        , numParity(_numParity)
        {
            int rows = numData + numParity;
            // could go as high as 255, but 250
            // is neater
            attest(rows <= 250, "Unable to create %i rows, limited to 250", rows);

            // create an array to calculate the parity
            d = makeArray(rows, numData + 1);
/*
            NEW AND IMPROVED
            based on original and updated papers

http://web.eecs.utk.edu/~plank/plank/papers/CS-03-504.pdf
http://web.eecs.utk.edu/~plank/plank/papers/CS-03-504.pdf

            Start with a "Vandermode" matrix, which is guaranteed
            invertible if you reduce and numParity rows

            [0^0 0^1 0^2 ...   [1 0 0
            [1^0 1^1 1^2 ...   [1 1 1
            [2^0 2^1 2^2 ...   [1 2
            [3^0 3^1 3^2 ...   [1 3
            [4^0 4^1 4^2 ...   [1 4

*/
            // first row, add 1 (makeArray initialises to)
            d[0][0] = 1;
            // second row all 1
            for (int col = 0; col < numData; col++)
            {
                d[1][col] = 1;
            }
            // third and successive rows
            for (int row = 2; row < rows; row++)
            {
                // first column, all 1
                d[row][0] = 1;
                // second column, row number
                d[row][1] = row;
                // rest of the columns, powers of "row"
                for (int col = 2; col < numData; col++)
                {
                    d[row][col] = gfa.mult(d[row][col-1], row);
                }
            }
            print("Vandermonde", dumpFile);
/*
            now we have to apply "elementary operations"
            to reduce the top part into an identity matrix
            first row is already in the right format
*/
            for (int row = 1; row < numData; ++row)
            {
                // we need to make d[row][row] == 1 and
                // the rest of row == 0 without disturbing the previous
                // rows.
                // first, ensure that d[row][row] is non-zero,
                // swap with a later column if needed
                if (!d[row][row])
                {
                    for (int col = row+1; col < numData; ++col)
                    {
                        if (!d[row][col]) continue;
                        // found a candidate column to swap with
                        for (int idx = row; idx < rows; ++idx)
                        {
                            uint8_t tmp = d[idx][row];
                            d[idx][row] = d[idx][col];
                            d[idx][col] = tmp;
                        }
                        break;
                    }
//                    print("swapped...");
                }
                // scale if necessary to ensure a major diagonal of 1
                if (d[row][row] != 1)
                {
                    uint8_t inv = gfa.div(1,d[row][row]);
                    for (int col = 0; col < numData; ++col)
                    {
                        d[row][col] = gfa.mult(inv,d[row][col]);
                    }
//                    print("scaled...");
                }
                // now zero-out the other columns
                for (int col = 0; col < numData; ++col)
                {
                    // leave the major diagonal alone
                    if (row == col) continue;
                    // already zero?
                    if (!d[row][col]) continue;
                    // take away multiples of the row'th column
                    uint8_t mult = d[row][col];
                    for (int idx = row; idx < rows; ++idx)
                    {
                        d[idx][col] ^= gfa.mult(mult,d[idx][row]);
                    }
                }
//                print("reduced...");
            }
            print("Parity", dumpFile);
            // make sure we got it right...
            // identity matrix at the top
            for (int row = 0; row < numData; ++row)
            {
                for (int col = 0; col < numData; ++col)
                {
                    assert(d[row][col] == (row == col) ? 1 : 0);
                }
            }
            // the rest must not be zero
            for (int row = numData; row < rows; ++row)
            {
                for (int col = 0; col < numData; ++col)
                {
                    assert(d[row][col]);
                }
            }
        };

    // ye olde destructor
    virtual ~GFM()
        {
            free(d);
            d = 0;
        }

    // helper function to create a 2-dimensional array of
    // bytes that can be free'd with a single free().
    // More importantly, the rows are arranged such that
    // [n][cols] == [n+1][0] so we can read/write the
    // whole thing with a single call
    static uint8_t ** makeArray(size_t rows, size_t cols)
        {
            int numCells = rows * cols;
            // allocate enough memory for the backbone and the cells
            ssize_t size =
                (rows     * sizeof(uint8_t *)) +
                (numCells * sizeof(uint8_t));
            uint8_t ** ret = (uint8_t **)calloc(size,1);
            attest(ret, "Unable to create %u x %u matrix", rows, cols);

            // first row starts just after the backbone
            ret[0] = (uint8_t *)&ret[rows];
            // subsequent rows abut
            for (size_t i = 1; i < rows; i++)
            {
                ret[i] = ret[i-1] + cols;
            }
            return ret;
        }

    // calculate the parity bits for a whole block of data
    //  data [0..len-1][0..(numData+numParity-1]
    inline void parity(uint8_t ** data, size_t len)
        {
            // clear all the rows corresponding to the parity bytes
            memset(data[numData], 0, (len * numParity));
            // process the parity bytes one at a time
            for (int row = numData; row < (numData + numParity); row++)
            {
                // cycle through each data bit for each parity bit
                for (int col = 0; col < numData; col++)
                {
                    // process the data one byte at a time
                    for (size_t idx = 0; idx < len; idx++)
                    {
                        // row and col are fixed, compiler should be able
                        // to optimise this nicely
                        data[row][idx] ^= gfa.mult(data[col][idx], d[row][col]);
                    }
                }
            }
        }

    // calculate the parity for a single block of data
    inline void parity(uint8_t * data)
        {
            // output = matrix * data
            // the first numData elements of output are just the data
            // which is kind of boring, so let's just do the last bit
            uint8_t * parity = data + numData;
            for (int row = numData; row < (numData + numParity); row++)
            {
                *parity = 0;
                for (int col = 0; col < numData; col++)
                {
                    *parity ^= gfa.mult(data[col], d[row][col]);
                }
                parity++;
            }
        }

    // mark a data (or parity) set as failed.
    void failData(uint8_t idx)
        {
            assert(idx < (numData + numParity));
            d[idx][numData] = -1;
        }
    void failParity(uint8_t idx)
        {
            failData(idx + numData);
        }
    bool failed(uint8_t idx)
        {
            // -1 == failed
            if (d[idx][numData] == (uint8_t)-1) return true;
            // must be -1 or 0 ...
            assert(!d[idx][numData]);
            return false;
        }


    // print out the D matrix
    void print(const char * msg,
               std::ostream & os = std::cerr)
        {
            print(msg, d, numData + numParity, numData, os);
        }

    // print out a given matrix
    static void print(const char * msg,
                      uint8_t ** m,
                      uint8_t rows,
                      uint8_t cols,
                      std::ostream & os = std::cerr)
        {
            if (!os) return;
            os << msg << '\n';
            for (int row = 0; row < rows; row++)
            {
                for (int col = 0; col < cols; col++)
                {
                    os << '\t' << (int)m[row][col];
                }
                os << '\n';
            }
            os << std::endl;
        }

    // generate the recovery matrix
    uint8_t ** recovery()
        {
// print numData+1 cols            print("Remaining", dumpFile);

            // create an array to hold the recovery matrix
            uint8_t ** ret = makeArray(numData, numData + 1);
            // create an identity matrix...
            for (int idx = 0; idx < numData; idx++)
            {
                ret[idx][idx] = 1;
            }

            // create a temporary matrix for the
            // upcoming matrix inversion
            uint8_t ** tmp = makeArray(numData, numData);

            // when replacing a failed row, start at the end of the matrix
            uint8_t tst = numData + numParity;
            // fill in the tmp matrix from the available rows
            for (int row = 0; row < numData; row++)
            {
                // assume the row has not failed (i.e. just copy it)
                uint8_t cpy = row;
                // if the row has failed ...
                if (failed(cpy))
                {
                    // search for a non-failed row to replace it
                    while(failed(--tst))
                    {
                        // make sure we haven't run out of redundancy..
                        assert(tst > (row + 1));
                    }
                    cpy = tst;
                }
                // copy the row
                memcpy(tmp[row], d[cpy], numData * sizeof(uint8_t));
                ret[row][numData] = cpy;
            }

            print("Recovery", tmp, numData, numData, dumpFile);

            // OK.... now I have to do a gaussian elimination on the tmp matrix

            // first reduce it to major column order
            for (int col = 0; col < numData-1; col++)
            {
                attest(tmp[col][col],
                       "zero in major diagonal[%d] of reduced", col);
                uint8_t ref = tmp[col][col];
                for (int row = col+1; row < numData; row++)
                {
                    uint8_t val = tmp[row][col];
                    // if this field is already zero then skip to the next one
                    if (!val) continue;
                    //	    DMP((int)val);
                    uint8_t mult = gfa.div(ref, val);
                    //tmp[row] *= mult
                    MulyRowBy(tmp, row, mult);
                    MulyRowBy(ret, row, mult);
                    //tmp[row] += tmp[col]
                    AddRow(tmp, row, col);
                    AddRow(ret, row, col);
                }
            }

            print("MCO", ret, numData, numData+1, dumpFile);

            // next... we'll reduce the upper triangle
            for (int col = 1; col < numData; col++)
            {
                attest(tmp[col][col],
                       "zero in major diagonal[%d] of MCO", col);
                uint8_t ref = tmp[col][col];
                for (int row = 0; row < col; row++)
                {
                    uint8_t val = tmp[row][col];
                    // if this field is already zero then skip to the next one
                    if (!val) continue;
                    //	    DMP((int)val);
                    uint8_t mult = gfa.div(ref, val);
                    //tmp[row] *= mult
                    MulyRowBy(tmp, row, mult);
                    MulyRowBy(ret, row, mult);
                    //tmp[row] += tmp[col]
                    AddRow(tmp, row, col);
                    AddRow(ret, row, col);
                }
            }

            print("UT", ret, numData, numData, dumpFile);

            // now normalise
            for (int idx = 0; idx < numData; idx++)
            {
                uint8_t mult = gfa.div(1,tmp[idx][idx]);
                MulyRowBy(tmp, idx, mult);
                MulyRowBy(ret, idx, mult);
            }

            print("Norm", ret, numData, numData, dumpFile);

            // OK.... now if we got that right then
            // tmp * ret = ret * tmp = I (mostly)
            // for failed rows, all bets are off
            for (int row = 0; row < numData; row++)
            {
                bool f = failed(row);
                for (int col = 0; col < numData; col++)
                {
                    uint8_t a = 0;
                    uint8_t b = 0;
                    for (int i = 0; i < numData; i++)
                    {
                        a ^= gfa.mult(tmp[row][i], ret[i][col]);
                        b ^= gfa.mult(ret[row][i], tmp[i][col]);
                    }
                    // assert((tmp * ret) == (ret * tmp))
                    assert(a == b);
                    // non-failed rows must be from the identity matrix
                    // failed rows must be fully populated
                    //a = ret[row][col];
//                    assert(f ? a : (a == (row == col) ? 1 : 0));
                    assert(f || (a == (row == col) ? 1 : 0));
                }
            }
            // get rid of the temp matrix and return the recovery one
            free(tmp);
            return ret;
        }

    // recover a block of data
    inline void recover(uint8_t ** data, uint8_t ** r, size_t len)
        {
            for (uint8_t row = 0; row < numData; row++)
            {
                // if this row is available ...
                if (r[row][numData] == row)
                {
                    // noting else to do
                    continue;
                }
                // nuke whatever junk there may be
                memset(data[row], 0, len);
                for (uint8_t col = 0; col < numData; col++)
                {
                    // row and col are constant now, I'm
                    // hoping the compiler is smart enough
                    // to optimise the following:
                    for (size_t idx = 0; idx < len; idx++)
                    {
                        data[row][idx] ^= gfa.mult(r[row][col],
                                                   data[r[col][numData]][idx]);
                    }
                }
            }
        }

    // recover a single dataset
    inline void recover(uint8_t * data, uint8_t ** r)
        {
            for (uint8_t row = 0; row < numData; row++)
            {
                uint8_t tmp = 0;
                for (uint8_t col = 0; col < numData; col++)
                {
                    tmp ^= gfa.mult(r[row][col],
                                    data[r[col][numData]]);
                }
                data[row] = tmp;
            }
        }

    // helper function for recovery matrix creation
    void MulyRowBy(uint8_t ** m, uint8_t row, uint8_t mult)
        {
            // cheating!! dim should be passed in!!
            for (int col = 0; col < numData; col++)
            {
                m[row][col] = gfa.mult(m[row][col], mult);
            }
        }

    // a += b
    void AddRow(uint8_t ** m, uint8_t a, uint8_t b)
        {
            for (int col = 0; col < numData; col++)
            {
                m[a][col] ^= m[b][col];
            }
        }

private:
    GFA        gfa;
    uint8_t ** d;
    uint8_t numData;
    uint8_t numParity;


public:
    // built-in test
    static void BIT()
        {
            // test 25 data blocks (64K each) with 25 parity blocks
            const uint8_t numData   = 25;
            const uint8_t numParity = 25;
            const size_t blockSize  = 64 * 1024;

            GFM gfm(numData, numParity);

            // run the GFA built-in-test
            gfm.gfa.BIT();

            // single row test (redundant?)
            uint8_t data[(numData+numParity)] = {55, 42, 69};

            // matrix test
            uint8_t ** data2 = gfm.makeArray(numData + numParity, blockSize);
            // fill the matrix with deterministic junk
            for (uint8_t rowIdx = 0; rowIdx < numData; rowIdx++)
            {
                uint8_t * row = data2[rowIdx];
                for (size_t idx = 0; idx < blockSize; idx++)
                {
                    row[idx] = (uint8_t)(idx * (rowIdx^idx));
                }
            }

            // generate the parity data
            gfm.parity(data);
            gfm.parity(data2, blockSize);

            // fail a bunch of rows
#define FAIL_DATA(x) {gfm.failData(x); data[x] = -2;}
            FAIL_DATA(9);
            FAIL_DATA(1);
            FAIL_DATA(2);
            FAIL_DATA(3);
            FAIL_DATA(4);
            FAIL_DATA(5);
            FAIL_DATA(6);
            FAIL_DATA(7);
#undef FAIL_DATA

            // generate a recovery matrix
            uint8_t ** r = gfm.recovery();

            // recover ...
            gfm.recover(data, r);
            gfm.recover(data2, r, blockSize);

            // test the junk
            for (uint8_t rowIdx = 0; rowIdx < numData; rowIdx++)
            {
                uint8_t * row = data2[rowIdx];
                for (size_t idx = 0; idx < blockSize; idx++)
                {
                    assert(row[idx] == (uint8_t)(idx * (rowIdx^idx)));
                }
            }

            free(r);
            free(data2);
        };
};

std::string MakeFilename(const std::string & stub, int num)
{
    std::ostringstream o;
    o << (stub);
    o << std::setw(2) << std::setfill('0') << std::hex << num;

    return o.str();
}

void writeHeader(int fd, const signature & sig, EVP_MD_CTX & ctx)
{
    attest(write(fd,&_binary_gfm_tar_start,
                 _binary_gfm_tar_len) == (ssize_t)_binary_gfm_tar_len,
           "Unable to write tarball");
    EVP_DigestUpdate(&ctx, &_binary_gfm_tar_start,
               _binary_gfm_tar_len);

    attest(write(fd,&sig, sizeof(sig))
           == (ssize_t)sizeof(sig),
           "Unable to write signature");
    EVP_DigestUpdate(&ctx, &sig, sizeof(sig));

    static ssize_t len = 0;
    static char  * pad = 0;
    if (!pad)
    {
        len = _binary_gfm_tar_len + sizeof(sig) + BLOCKSIZE - 1;
        //DMP(_binary_gfm_tar_len);
        //DMP(sizeof(sig));
        //DMP(BLOCKSIZE);
        len &= ~(BLOCKSIZE - 1);
        len -= _binary_gfm_tar_len + sizeof(sig);
        //DMP(len + _binary_gfm_tar_len + sizeof(sig));
        //DMPX(len + _binary_gfm_tar_len + sizeof(sig));
        //DMP(len);
        pad = (char*)calloc(len,1);
        attest(pad, "unable to calloc pad");
    }
    attest(write(fd,pad, len) == len,
           "Unable to write pad");
    EVP_DigestUpdate(&ctx, pad, len);
}

ssize_t readFully(int fd, void * buff, ssize_t len)
{
    // previously read
    ssize_t prev = 0;
    // number of bytes read this (first) time
    ssize_t rc = read(fd, buff, len);
    while(rc > 0)
    {
        if ((rc + prev) == len)
	{
            // read it all, bye!
            return len;
	}
        prev += rc;
        rc = read(fd, ((char*)buff) + prev, len - prev);
    }
    // EOF or some other error, close the file!
    close(fd);
    return (rc < 0) ? rc : prev;
}

void addPadding(uint8_t * buff, ssize_t numRead, ssize_t expected)
{
    // is the buffer full?
    if (numRead == expected)
    {
        // no need to add padding!
        buff[expected] = 0;
        return;
    }
    // how many bytes are missing?
    ssize_t missing = (expected - numRead);
    // sanity check.....
    assert(missing > 0);

    // flag the block as being short
    // missing fewer than 0x80 (128) bytes?
    if (missing < 0x80)
    {
        // just use the last byte to store the shortfall
        buff[expected] = (uint8_t)(missing & 0xFF);
        return;
    }
    // use a 32-bit int to store the shortfall
    buff[expected--] = 0x80;
    uint32_t * buff32 = (uint32_t *)buff;
    buff32[(expected/4)-2] = missing;
}

size_t removePadding(uint8_t * buff, size_t buffSize)
{
    uint8_t flag = buff[--buffSize];
    // block full?
    if (flag == 0)
    {
        // no padding to remove
//        DMP(buffSize);
        return buffSize;
    }

    // block missing < 128 bytes?
    if (flag < 0x80)
    {
//        DMP(buffSize - flag);
        return buffSize - flag;
    }

    // missing lots!
    uint32_t * buff32 = (uint32_t *)buff;
    uint32_t missing = buff32[(buffSize/4)-2];
    buffSize -= missing;
//    DMP(missing);
//    DMP(buffSize);
    return buffSize;
}

std::string StripDir(const std::string & filename)
{
    size_t found = filename.find_last_of("/\\");
    // make sure fn doesn't get gc'ed which fn.c_str is in use!
    return filename.substr(found+1);
}

// print out the MD checksums
void PrintMD(FILE * file,
	     const std::string & filename,
	     EVP_MD_CTX & ctx)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digestLen = sizeof(digest);
    EVP_DigestFinal_ex(&ctx, digest, &digestLen);

    unsigned i;
    for (i = 0; i < digestLen; i++)
    {
        fprintf(file, "%02x", (digest[i] & 0xFF));
    }
    // make sure fn doesn't get gc'ed which fn.c_str is in use!
    std::string fn = StripDir(filename);
    fprintf(file, "  %s\n", fn.c_str());

    EVP_MD_CTX_cleanup(&ctx);
}

void CreateParity(const uint8_t numData,
		  const uint8_t numParity,
		  const std::string & stub)
{
    GFM gfm (numData, numParity);
    int fds[numParity + numData];
    signature sig;
    sig.numData = numData;
    sig.numParity = numParity;
    //  sig.fileNum = 0;;
    sig.blocksizePo2 = BLOCKSIZE_Po2;

    EVP_MD_CTX MD_ctx[257];
    std::string filename[257];

    const EVP_MD * EVP_MD5 = EVP_md5();

    EVP_MD_CTX_init(&MD_ctx[256]);
    EVP_DigestInit_ex(&MD_ctx[256], EVP_MD5, 0);
    filename[256] = stub + ".md5";
    FILE * md5File = fopen(filename[256].c_str(), "w");
    attest(md5File, "Unable to open MD file: '%s'",
           filename[256].c_str());

    for (int idx = 0; idx < (numData + numParity); idx++)
    {
        filename[idx] = MakeFilename(stub, idx);
        fds[idx] = open(filename[idx].c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC, 0644);
        attest(fds[idx], "Unable to open file: '%s'",
               filename[idx].c_str());

        EVP_MD_CTX_init(&MD_ctx[idx]);
        EVP_DigestInit_ex(&MD_ctx[idx], EVP_MD5, 0);

        sig.fileNum = idx;
        writeHeader(fds[idx], sig, MD_ctx[idx]);

    }

    uint8_t ** buff = GFM::makeArray(numData + numParity, BLOCKSIZE);

    while(1)
    {
        memset(buff[0], 0, numData * BLOCKSIZE);
        ssize_t numRead = readFully(0, buff[0], (numData * BLOCKSIZE) - 1);
        addPadding(buff[0], numRead, (numData * BLOCKSIZE) - 1);
        // calc parity
        gfm.parity(buff, BLOCKSIZE);

        EVP_DigestUpdate(&MD_ctx[256], buff[0], numRead);


        // write data/parity
        for (int idx = 0; idx < (numData + numParity); idx++)
	{
            ssize_t numWritten = write(fds[idx], buff[idx], BLOCKSIZE);
            attest(numWritten == (ssize_t)BLOCKSIZE,
                   "Unable to write block: '%s'",
                   filename[idx].c_str());

            EVP_DigestUpdate(&MD_ctx[idx], buff[idx], BLOCKSIZE);
	}
        // done?
        if (numRead != (ssize_t)((numData * BLOCKSIZE)-1))
	{
            break;
	}
    }

    // finish off all the files
    for (int idx = 0; idx < (numData + numParity); idx++)
    {
        close(fds[idx]);
        PrintMD(md5File, filename[idx], MD_ctx[idx]);
    }

    PrintMD(md5File, "-", MD_ctx[256]);

    fclose(md5File);

    free(buff);
}


int OpenFile(const std::string & filename,
	     signature & sig)
{
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return 0;
    }

    off_t off = lseek(fd, 124, SEEK_SET);
    attest((off == 124),
           "unable to seek to file size in tar header");

    char buff[12];
    ssize_t rc = read(fd, buff, 11);
    attest((rc == (ssize_t)11),
           "unable to read file size from tar header");
    buff[11] = '\0';

    char * endptr = 0;
    uint32_t s = strtol(buff, &endptr, 8);
    attest(endptr && (*endptr == '\0'),
           "unable to decode file size from tar header");
    s += 0x200;
    off = lseek(fd, s, SEEK_SET);
    attest((s == (uint32_t)off),
           "unable to seek to end of tar-blob");

    signature chk;
    rc = read(fd, &chk, sizeof(chk));
    attest((rc == sizeof(chk)),
           "unable to read signature block");
    // might not know numData yet either...
    if (sig.numData == 255)
    {
        sig.numData   = chk.numData;
        sig.numParity = chk.numParity;
    }
    else
    {
        // numParity not known at this stage, so cheat
        chk.numParity = sig.numParity;
    }
    // check that
    if (memcmp(&sig, &chk, sizeof(sig)))
    {
        close(fd);
        return 0;
    }

    // see to next BLOCKSIZE boundary
    off += sizeof(signature) + BLOCKSIZE - 1;
    off &= ~(BLOCKSIZE - 1);
    attest((lseek(fd, off, SEEK_SET) == off),
           "unable to seek to end of tar-blob (0x%x): %m", off);

    return fd;
}

int OpenFile(const std::string & filename,
	     uint8_t idx,
	     GFM & gfm,
	     signature & sig)
{
    int fd = OpenFile(filename, sig);
    if (fd)
    {
        return fd;
    }
    gfm.failData(idx);
    return 0;
}

void RecoverData(const uint8_t numData,
		 const uint8_t numParity,
		 GFM & gfm,
		 int * fds)
{
    uint8_t ** buff = GFM::makeArray(numData + numParity, BLOCKSIZE);
    uint8_t ** rcvr = gfm.recovery();

    while(1)
    {
        memset(buff[0], 0, (numData + numParity) * BLOCKSIZE);

        ssize_t numRead = 0;

        for (int idx = 0; idx < (numData + numParity); idx++)
	{
            if (fds[idx])
	    {
                numRead += readFully(fds[idx], buff[idx], BLOCKSIZE);
	    }
	}

        if (numRead <= 0)
	{
            break;
	}
        gfm.recover(buff, rcvr, BLOCKSIZE);

        size_t numToWrite = removePadding(buff[0], numData * BLOCKSIZE);

        size_t rc = write(1, buff[0], numToWrite);//numData * BLOCKSIZE);
        attest(rc == numToWrite, "Expected to write %zd, wrote %zd", numToWrite, rc);
    }

    for (int idx = 0; idx < (numData + numParity); idx++)
    {
        close(fds[idx]);
    }

    free(buff);
    free(rcvr);
}

/**
   Recover given only the filename stub.
*/
void RecoverData(const std::string & stub)
{
    int fds[250] = {0,};

    // use this to make sure all the files have the same
    // parameters
    signature expected = {0,0,0,0};
    signature sig;
    sig.numData   = 255;
    sig.numParity = 255;
    //  sig.fileNum = 0;;
    sig.blocksizePo2 = BLOCKSIZE_Po2;

    for (int idx = 0; idx < 250; idx++)
    {
        sig.fileNum = idx;
        std::string filename =  MakeFilename(stub, idx);
        fds[idx] = OpenFile(filename, sig);
        if (fds[idx] > 0)
	{
            if (!expected.fileNum++)
	    {
                expected.numData   = sig.numData;
                expected.numParity = sig.numParity;
                expected.blocksizePo2 = sig.blocksizePo2;
                continue;
	    }

            attest(expected.numData   == sig.numData,
                   "signature.numData inconsistent: %s",
                   filename.c_str());
            attest(expected.numParity == sig.numParity,
                   "signature.numParity inconsistent: %s",
                   filename.c_str());
            attest(expected.blocksizePo2 == sig.blocksizePo2,
                   "signature.blocksizePo2 inconsistent: %s",
                   filename.c_str());
            if (expected.fileNum < sig.numData)
	    {
                continue;
	    }
            break;
	}
    }
    // did we manage to open any files?
    if (!expected.fileNum)
    {
        int fd = (stub == "-")
            ? STDOUT_FILENO
            : open(stub.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL,
                   0644);
        attest(fd >= 0,
               "Unable to open %s: %d (%s)",
               stub.c_str(), errno, strerror(errno));
        size_t s = _binary_gfm_tar_len - 0x200;
        size_t numWritten = write(
            fd,
            &_binary_gfm_tar_start + 0x200, s);
        attest(numWritten == s,
               "only wrote %zd of %zd to %s",
               numWritten, s,
               stub.c_str());
        close(fd);
        std::cerr << "Wrote all "
                  << s
                  << " bytes of .tar.xz data to "
                  << stub << std::endl;
        exit(0);
    }

    const uint8_t numData   = sig.numData;
    const uint8_t numParity = sig.numParity;
    attest((numData + numParity) <= 250,
           "Signature invalid, number of files (data + parity) "
           "must not exceed 250: '%s'", stub.c_str());

    attest(expected.fileNum >= numData,
           "Unable to recover, need at least %i files available: '%s'",
           numData, stub.c_str());

    GFM gfm (numData, numParity);

    for (int idx = 0; idx < (numData + numParity); idx++)
    {
        if (!fds[idx])
	{
            gfm.failData(idx);
	}
    }

    // now that we have opened all the files, start the recovery.
    RecoverData(numData,
                numParity,
                gfm,
                fds);
}

void rtfm(const std::string & prog)
{
    std::cerr << "\t# " GIT_TAG "\n"
              << prog <<
        " STUB [NUM_DATA NUM_PARITY]\n"
        "\tSTUB         filename stub for files\n"
        "\tNUM_DATA     number of data files\n"
        "\tNUM_PARITY   number of parity files\n"
              << prog <<
        "\tDUMP.tar.xz  dump embedded data\n"
              << std::endl;
    exit(1);
}

int main(int argc, char ** argv)
{
    // Execute built-in test, verbose if requested
    if(getenv("BIT"))
    {
        std::cerr << "BIT ..." << std::endl;
        GFM::BIT();
        std::cerr << "BIT OK!" << std::endl;
    }

    if (getenv("DMP"))
    {
        std::string filename = StripDir(argv[argc > 1 ? 1 : 0]);
        {
            std::string fn = filename + ".gfa";
            std::ofstream os(fn.c_str());
            GFA gfa;
            gfa.operator<<(os);
        }
        filename += ".gfm";
        dumpFile.open(filename.c_str());
    }

    // recovery.
    // Specify the file stub
    if (argc == 2)
    {
        RecoverData(argv[1]);
        exit(0);
    }

    // generation mode.
    // Specify file stub, number of data and number
    // of parity files to split into.
    if (argc == 4)
    {
        int numData   = atoi(argv[2]);
        int numParity = atoi(argv[3]);

        if (numData < 0)
        {
            _binary_gfm_tar_len = 0;
            numData = -numData;
        }
        attest((numData > 0) && (numData < 250),
               "You must specify between 1 and 249 data files");
        attest((numParity > 0) && (numParity < 250),
               "You must specify between 1 and 249 parity files");
        attest((numData + numParity) <= 250,
               "Number of files (data + parity) must not exceed 250");

        CreateParity(numData, numParity, argv[1]);
        exit(0);
    }

    rtfm(argv[0]);
    return 0;
}
