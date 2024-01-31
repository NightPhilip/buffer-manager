// Nattarach Larptaweepornsup, larptaweepor@wisc.edu
// Tatchapol Singhathongpoon, jettanachai@wisc.edu
// Haoyang Huang, hhuang433@wisc.edu


#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <iostream>
#include <stdio.h>
#include "page.h"
#include "buf.h"

#define ASSERT(c)  { if (!(c)) { \
		       cerr << "At line " << __LINE__ << ":" << endl << "  "; \
                       cerr << "This condition should hold: " #c << endl; \
                       exit(1); \
		     } \
                   }

//----------------------------------------
// Constructor of the class BufMgr
//----------------------------------------

BufMgr::BufMgr(const int bufs)
{
    numBufs = bufs;

    bufTable = new BufDesc[bufs];
    memset(bufTable, 0, bufs * sizeof(BufDesc));
    for (int i = 0; i < bufs; i++) 
    {
        bufTable[i].frameNo = i;
        bufTable[i].valid = false;
    }

    bufPool = new Page[bufs];
    memset(bufPool, 0, bufs * sizeof(Page));

    int htsize = ((((int) (bufs * 1.2))*2)/2)+1;
    hashTable = new BufHashTbl (htsize);  // allocate the buffer hash table

    clockHand = bufs - 1;
}


BufMgr::~BufMgr() {

    // flush out all unwritten pages
    for (int i = 0; i < numBufs; i++) 
    {
        BufDesc* tmpbuf = &bufTable[i];
        if (tmpbuf->valid == true && tmpbuf->dirty == true) {

#ifdef DEBUGBUF
            cout << "flushing page " << tmpbuf->pageNo
                 << " from frame " << i << endl;
#endif

            tmpbuf->file->writePage(tmpbuf->pageNo, &(bufPool[i]));
        }
    }

    delete [] bufTable;
    delete [] bufPool;
}

/// @brief Allocates a free frame using the clock algorithm
/// @param frame address to put frame number of newly allocated page
/// @return Status OK if success else BUFFEREXCEEDED or UNIXERR. Return frame number via frame param
const Status BufMgr::allocBuf(int & frame) 
{
    int initialClockHand = clockHand; //Initial clockhand location
    int first = 1;

    while (true)
    {
        advanceClock(); // Advance clockhand

        BufDesc* tmpbuf = &bufTable[clockHand];
        
        // check if recently ref
        if (tmpbuf->refbit) 
        {
            tmpbuf->refbit = false;
            continue;
        }

        // check if pinned
        if (tmpbuf->pinCnt > 0) 
        {
            if (clockHand == initialClockHand) 
            {   
                // check if first time
                if (first == 1){
                    first = 0;
                    continue;
                }
                return BUFFEREXCEEDED;
            }
            continue;
        }

        // found a page to evict so check if need to write back to disk
        if (tmpbuf->dirty) 
        {
            Status writeStatus = tmpbuf->file->writePage(tmpbuf->pageNo, &(bufPool[clockHand]));
            if (writeStatus != OK) 
            {
                return UNIXERR;
            }
            tmpbuf->dirty = false;
        }

        // check if need to remove from hashtable
        if (tmpbuf->valid) 
        {
            hashTable->remove(tmpbuf->file, tmpbuf->pageNo);
        }

        frame = clockHand;
        return OK;
    }
}


/// @brief Decrements the pinCnt of the frame containing(file,pageNo), and if the page is dirty, sets the dirty bit
/// @param file pointer to the file which the page that needs to be unpinned belongs to
/// @param PageNo the page number of the page that needs to be unpinned
/// @param dirty indicates if the page is dirty
/// @return STATUS OK if no errors occurred, HASHNOTFOUND if the page is not in the buffer pool hash table, PAGENOTPINNED if the pin count is already 0
const Status BufMgr::readPage(File* file, const int PageNo, Page*& page) 
{
    int frameNo;
    Status lookupStatus = hashTable->lookup(file, PageNo, frameNo);

    // Case 1: Page is in the buffer pool
    if (lookupStatus == OK) 
    {
        bufTable[frameNo].refbit = true;
        bufTable[frameNo].pinCnt++;
        page = &bufPool[frameNo];
        return OK;
    }

    if (lookupStatus == HASHNOTFOUND) 
    {
        Status allocStatus = allocBuf(frameNo);
        if (allocStatus != OK) 
        {
            return allocStatus;
        }

        Status readStatus = file->readPage(PageNo, &bufPool[frameNo]);
        if (readStatus != OK) 
        {
            return UNIXERR;
        }

        hashTable->insert(file, PageNo, frameNo);

        bufTable[frameNo].Set(file, PageNo);
        page = &bufPool[frameNo];
        return OK;
    }

    return HASHTBLERROR;
}


/// @brief Decrements the pinCnt of the frame containing(file,pageNo), and if the page is dirty, sets the dirty bit
/// @param file pointer to the file which the page that needs to be unpinned belongs to
/// @param PageNo the page number of the page that needs to be unpinned
/// @param dirty indicates if the page is dirty
/// @return STATUS OK if no errors occurred, HASHNOTFOUND if the page is not in the buffer pool hash table, PAGENOTPINNED if the pin count is already 0
const Status BufMgr::unPinPage(File* file, const int PageNo, const bool dirty) 
{
    int frameNo;
    Status lookupStatus = hashTable->lookup(file, PageNo, frameNo);

    if (lookupStatus == HASHNOTFOUND) 
    {
        return HASHNOTFOUND;
    }

    if (bufTable[frameNo].pinCnt == 0) 
    {
        return PAGENOTPINNED;
    }

    bufTable[frameNo].pinCnt--;

    if (dirty) 
    {
        bufTable[frameNo].dirty = true;
    }

    return OK;
}

/// @brief Allocate an empty page for a file and allocate a buffer for it
/// @param file file pointer of file to allocate new page
/// @param PageNo Address to put in newly created page number of file
/// @param page Address to put pointer to buffer of newly allocated page
/// @return Status OK if success, UNIXERR, BUFFEREXCEEDED, or HASHTBLERROR if error occur. Return new page number and pointer buffer via PageNo and page argument
const Status BufMgr::allocPage(File* file, int& PageNo, Page*& page) 
{
    Status allocPageStatus = file->allocatePage(PageNo);
    if (allocPageStatus != OK) 
    {
        return UNIXERR;
    }

    int frameNo;
    Status allocBufStatus = allocBuf(frameNo);
    if (allocBufStatus != OK) 
    {
        return allocBufStatus;
    }

    Status insertStatus = hashTable->insert(file, PageNo, frameNo);
    if (insertStatus != OK) 
    {
        return HASHTBLERROR;
    }

    bufTable[frameNo].Set(file, PageNo);
    page = &bufPool[frameNo];

    return OK;
}


const Status BufMgr::disposePage(File* file, const int pageNo) 
{
    Status status = OK;
    int frameNo = 0;
    status = hashTable->lookup(file, pageNo, frameNo);
    if (status == OK)
    {
        bufTable[frameNo].Clear();
    }
    status = hashTable->remove(file, pageNo);

    return file->disposePage(pageNo);
}

const Status BufMgr::flushFile(const File* file) 
{
  Status status;

  for (int i = 0; i < numBufs; i++) {
    BufDesc* tmpbuf = &(bufTable[i]);
    if (tmpbuf->valid == true && tmpbuf->file == file) {

      if (tmpbuf->pinCnt > 0)
	  return PAGEPINNED;

      if (tmpbuf->dirty == true) {
#ifdef DEBUGBUF
	cout << "flushing page " << tmpbuf->pageNo
             << " from frame " << i << endl;
#endif
	if ((status = tmpbuf->file->writePage(tmpbuf->pageNo,
					      &(bufPool[i]))) != OK)
	  return status;

	tmpbuf->dirty = false;
      }

      hashTable->remove(file,tmpbuf->pageNo);

      tmpbuf->file = NULL;
      tmpbuf->pageNo = -1;
      tmpbuf->valid = false;
    }

    else if (tmpbuf->valid == false && tmpbuf->file == file)
      return BADBUFFER;
  }
  
  return OK;
}


void BufMgr::printSelf(void) 
{
    BufDesc* tmpbuf;
  
    cout << endl << "Print buffer...\n";
    for (int i=0; i<numBufs; i++) {
        tmpbuf = &(bufTable[i]);
        cout << i << "\t" << (char*)(&bufPool[i]) 
             << "\tpinCnt: " << tmpbuf->pinCnt;
    
        if (tmpbuf->valid == true)
            cout << "\tvalid\n";
        cout << endl;
    };
}


