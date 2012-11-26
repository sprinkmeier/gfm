#include "gfa.hh"

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <iostream>
#include <string>
#include <sstream>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>

extern char _binary_blob_bin_start;
extern char _binary_blob_bin_end;

size_t  _binary_blob_bin_len = &_binary_blob_bin_end - &_binary_blob_bin_start;

/// needs to be the same for parity gerneration and recovery.
/// Choose multiples of 512 'cos that's one disk sector.
/// larger values _might_ make it go faster
/// but might waste more on partial blocks
const uint8_t BLOCKSIZE_Po2 = 12;
const size_t  BLOCKSIZE     = 1 << BLOCKSIZE_Po2;
const size_t  MAGIC         = 64;

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
    d = makeArray(rows, numData);

    // the identity part of the matrix.
    for (int idx = 0 ; idx < numData; idx++)
      {
	d[idx][idx] = 1;
      }

    // first column of the parity part of the matrix, all 1
    for (int row = numData; row < rows; row++)
      {
	d[row][0] = 1;
      }

    // calculte the rest of the parity part of the matrix
    for (int col = 1; col < numData; col++)
      {
	uint8_t base = col+1;
	d[numData][col] = 1;
	for (int row = numData+1; row < rows; row++)
	  {
	    d[row][col] = gfa.mult(base, d[row-1][col]);
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
  void parity(uint8_t ** data, size_t len)
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
  void parity(uint8_t * data)
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
  // all vaid rows have a 0 or 1 in the first column
  // so use 0xFF as a marker
  void failData(uint8_t idx)
  {
    assert(idx < (numData + numParity));
    d[idx][0] = -1;
  }
  void failParity(uint8_t idx)
  {
    failData(idx + numData);
  }

  // print out the D matrix
  void print(const char * msg)
  {
    print(msg, d, numData + numParity, numData);
  }

  // print out a given matrix
  static void print(const char * msg,
		    uint8_t ** m,
		    uint8_t rows,
		    uint8_t cols)
  {
    std::cerr << msg << '\n';
    for (int row = 0; row < rows; row++)
      {
	for (int col = 0; col < cols; col++)
	  {
	    std::cerr << '\t' << (int)m[row][col];
	  }
	std::cerr << '\n';
      }
    std::cerr << std::endl;
  }

  // generate the recovery matrix
  uint8_t ** recovery()
  {
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
	if (d[cpy][0] == (uint8_t)-1)
	  {
	    // make sure we haven't run out of redundancy..
	    assert(tst > row);
	    // search for a non-failed row to replace it
	    while(--tst && (d[tst][0] == (uint8_t)-1));
	    cpy = tst;
	  }
	// copy the row
	memcpy(tmp[row], d[cpy], numData * sizeof(uint8_t));
	ret[row][numData] = cpy;
      }

    // OK.... now I have to do a gaussian elimination on the tmp matrix

    // first reduce it to major column order
    for (int col = 0; col < numData-1; col++)
      {
	assert(tmp[col][col]);
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

    // next... we'll reduce the upper triangle
    for (int col = 1; col < numData; col++)
      {
	assert(tmp[col][col]);
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

    // now normalise
    for (int idx = 0; idx < numData; idx++)
      {
	uint8_t mult = gfa.div(1,tmp[idx][idx]);
	MulyRowBy(tmp, idx, mult);
	MulyRowBy(ret, idx, mult);
      }


    // OK.... now if we got that right then
    // tmp * ret = ret * tmp = I (mostly)
    // for failed rows, all bets are off
    for (int row = 0; row < numData; row++)
      {
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
	    // unless it's a failed row, assert ((ret * tmp) == I)
	    if (d[row][0] != (uint8_t)-1)
	      {
		assert (a == (row == col) ? 1 : 0);
	      }
	  }
      }
    // get rid of the temp matrix and return the recovery one
    free(tmp);
    return ret;
  }

  // recover a block of data
  void recover(uint8_t ** data, uint8_t ** r, size_t len)
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
  void recover(uint8_t * data, uint8_t ** r)
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
    const uint8_t numData   =  25;
    const uint8_t numParity =  25;
    const size_t blockSize = 64 * 1024;

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
  o << (stub) << num;

  return o.str();
}

void writeHeader(int fd, const signature & sig, EVP_MD_CTX & ctx)
{
  // This static buffer is reused between files.
    static std::string buff;
//  static char * buff = 0;
  static uint32_t s = BLOCKSIZE;

  if (!buff.length())
    {

      unsigned t = (64 + _binary_blob_bin_len) / BLOCKSIZE;

      while(t)
	{
	  t >>= 1;
	  s <<= 1;
	}

      // magic here!!
      buff = "dd bs=64 skip=1 < FILE | unxz > gfm.tar"
	     "\n\n\n\n\n\n\n\n\n\n\n\n";
      buff.resize(s);
      memcpy(&buff[MAGIC],
             &_binary_blob_bin_start,
             _binary_blob_bin_len);
    }
  memcpy(&buff[MAGIC-8], &s,   4);
  memcpy(&buff[MAGIC-4], &sig, 4);
  attest(write(fd,&buff[0],s) == (ssize_t)s,
	 "Unable to write signature");
  EVP_DigestUpdate(&ctx, &buff[0], s);

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
      return buffSize;
    }

  // block missing < 128 bytes?
  if (flag < 0x80)
    {
      return buffSize - flag;
    }

  // missing lots!
  uint32_t * buff32 = (uint32_t *)buff;
  return buffSize - buff32[(buffSize/4)-2];
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
  fprintf(file, "  %s\n", filename.c_str());

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
      // calc parity
      gfm.parity(buff, BLOCKSIZE);

      EVP_DigestUpdate(&MD_ctx[256], buff[0], numRead);

      addPadding(buff[0], numRead, (numData * BLOCKSIZE) - 1);

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
	     uint8_t idx,
	     signature & sig)
{
  int fd = open(filename.c_str(), O_RDONLY);
  if (fd < 0)
    {
      return 0;
    }

  char buff[BLOCKSIZE];
  ssize_t rc = read(fd, buff, BLOCKSIZE);
  if (rc != (ssize_t)BLOCKSIZE)
    {
      close(fd);
      return 0;
    }

  uint32_t s = *((uint32_t*)(buff+MAGIC-8));
  signature & chk(*((signature*)(buff+MAGIC-4)));
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

  if(lseek(fd, s, SEEK_SET) != (off_t)s)
    {
      close(fd);
      return 0;
    }

  return fd;
}

int OpenFile(const std::string & filename,
	     uint8_t idx,
	     GFM & gfm,
	     signature & sig)
{
  int fd = OpenFile(filename, idx, sig);
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

      write(1, buff[0], numToWrite);//numData * BLOCKSIZE);
    }

  for (int idx = 0; idx < (numData + numParity); idx++)
    {
      close(fds[idx]);
    }

  free(buff);
}

/**
   Recover given only the filename stub.
 */
void RecoverData(const std::string & stub)
{
  int fds[250];

  // use this to make sure all the files have the same
  // parameters
  signature expected = {0,0,0};
  signature sig;
  sig.numData   = 255;
  sig.numParity = 255;
  //  sig.fileNum = 0;;
  sig.blocksizePo2 = BLOCKSIZE_Po2;

  for (int idx = 0; idx < 250; idx++)
    {
      sig.fileNum = idx;
      std::string filename =  MakeFilename(stub, idx);
      fds[idx] = OpenFile(filename, idx, sig);
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
  attest(expected.fileNum, "Unable to find any files: '%s'",
	 stub.c_str());

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
  // svn propset svn:keywords "Author Date Id Revision" gfm.cc


  std::cerr << "\t# $Id: gfm.cc 23 2008-08-29 22:42:09Z sprinkmeier $\n"
	    << prog <<
    " STUB [NUM_DATA NUM_PARITY]\n"
    "\tSTUB       filename stub for files\n"
    "\tNUM_DATA   number of data files\n"
    "\tNUM_PARITY number of parity files (implies generate mode)\n"
	    << std::endl;
  exit(1);
}

int main(int argc, char ** argv)
{
  // Execure built-in test if requested
  if (getenv("BIT"))
  {
    std::cerr << "BIT ..." << std::endl;
    GFA gfa;
    gfa.BIT();
    GFM::BIT();
    std::cerr << "BIT OK!" << std::endl;
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
