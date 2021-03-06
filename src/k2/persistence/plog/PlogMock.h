/*
MIT License

Copyright(c) 2020 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#pragma once

#include <map>
#include <random>
#include <string>
#include <fstream>
#include <filesystem>

#include <seastar/core/seastar.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/align.hh>

#include <k2/common/Common.h>
#include "IPlog.h"


#ifdef EXPOSE_PRIVATES
#define PRIVATE public
#else
#define PRIVATE private
#endif

namespace k2
{

struct PlogIdComp {
    bool operator() (const PlogId& lhs, const PlogId& rhs) const
    {
        uint i=0;
        while (i<PLOG_ID_LEN && lhs.id[i]==rhs.id[i]) {
            i++;
        }

        return (i<PLOG_ID_LEN) ? lhs.id[i]<rhs.id[i] : false;
    }
};


struct PlogFileDescriptor
{
    seastar::file ssfile;                  // plog file descriptor
    Binary headBuffer{DMA_ALIGNMENT}; // sync up first 4k(=DMA_ALIGNMENT) with plog file
    Binary tailBuffer{DMA_ALIGNMENT}; // sync up last 4k(=DMA_ALIGNMENT) with plog file when plog size (head+log_blocks) is not aligned.

    PlogFileDescriptor()
    {
        memset(headBuffer.get_write(), (int)0, DMA_ALIGNMENT);
        memset(tailBuffer.get_write(), (int)0, DMA_ALIGNMENT);
    }

    PlogInfo& getInfo() { return *(PlogInfo*)headBuffer.get_write(); }
    const PlogInfo& getInfo() const { return *(const PlogInfo*)headBuffer.get(); }

    size_t getTailOffset() const { return seastar::align_down(getInfo().size, DMA_ALIGNMENT); }
    size_t getTailOffsetInFile() const { return getTailOffset() + DMA_ALIGNMENT; }
    size_t getTailSize() const { return (size_t)getInfo().size - getTailOffset(); }
    size_t getTailRemaining() const { return DMA_ALIGNMENT - getTailSize(); }

    DEFAULT_MOVE(PlogFileDescriptor);
    seastar::future<> close();
};

class PlogMock : public IPlog {
public:
    /**
     * create plog files with head information (plog info) only, the file name is identified by plog id
     * a plog file is created using seastar file system, it must be aligned to dma_alignment(4k currently).
     *   -------------------------------------------------------
     *   | head |        body (plog blocks)        | padding |
     *   -------------------------------------------------------
     * plog file is composed of:
     *   - head: 8 bytes of plog information {size, sealed}
     *           - size: uint32_t, the number of bytes of head and plog blocks
     *           - sealed: bool, indicates the plog is sealed or not
     *   - body: plog blocks
     *   - padding: align to dma_alignment
     *
     * plogCount - the number of plog files to create
     * return - a list of unique plog ids that created
     **/
    seastar::future<std::vector<PlogId>> create(uint plogCount) override;

    /**
     * retrieve the plog information for given plog id:
     *    if plog id exists in m_plogFileDescriptors, retrieve the plog information directly from the map list
     *    otherwise, load plog information from plog file to m_plogFileDescriptors if the plog file for
     *    the given plog id exists, then return its plog information.
     *
     * plogId - the plog id to retrieve
     * return - the plog information, including plog size and sealing flag
     * Exception - throw an PlogException with status of P_PLOG_ID_NOT_EXIST if the given plog id doesn't exist.
     **/
    seastar::future<PlogInfo> getInfo(const PlogId& plogId) override;

    /**
     * append a list of buffers to plog file for given plog id
     *
     * plogId - plog id for append
     * bufferList - a list of log blocks to append
     * return - the file position starting to append log blocks
     * Exception - throw an PlogException with the status in following cases
     *   - P_PLOG_ID_NOT_EXIST if the given plog id doesn't exist
     *   - P_PLOG_SEALED if plog file is sealed
     *   - P_EXCEED_PLOGID_LIMIT if the total bytes of write buffers exceeds plog limit
     **/
    seastar::future<uint32_t> append(const PlogId& plogId, Binary buffer) override;

    /**
     * retrive a set of plog blocks for given plog id
     *
     * plogId - plog id for append
     * plogDataToReadList - a list of regions to read
     * return - a list of regions that include plog blocks read from plog file
     * Exception - throw an PlogException with the status in following cases
     *   - P_PLOG_ID_NOT_EXIST if the given plog id doesn't exist
     *   - P_CAPACITY_NOT_ENOUGH if the bytes in the plog file is not enough to read
     */
    seastar::future<ReadRegion> read(const PlogId& plogId, ReadRegion region)  override;

    /**
     * seal plog file for given plog id
     *
     * plogId - plog id to seal
     * return - no return
     * Exception - throw an PlogException with status of P_PLOG_ID_NOT_EXIST if the given plog id doesn't exist
    */
    seastar::future<> seal(const PlogId& plogId) override;

    /**
     * drop plog file for given plog id
     *
     * plogId - plog id to drop
     * return - no return,
     * Exception - throw an PlogException with status of P_PLOG_ID_NOT_EXIST if the given plog id doesn't exist
     */
    seastar::future<> drop(const PlogId& plogId) override;

    void setPlogMaxSize(uint32_t plogMaxSize) {
        m_plogMaxSize = plogMaxSize;
    }

    void setPlogFileNamePrefix(String plogFileNamePrefix) {
        m_plogFileNamePrefix = plogFileNamePrefix;
    }

    uint32_t getPlogMaxSize() {
        return m_plogMaxSize;
    }

    String getPlogPath() {
        return m_plogPath;
    }

    String getPlogFileNamePrefix() {
        return m_plogFileNamePrefix;
    }

    PlogMock(String plogPath = String("./plogData"));

    ~PlogMock();

    seastar::future<> close() override;

PRIVATE:

    seastar::future<PlogFileDescriptor*> getDescriptor(const PlogId& plogId);

    PlogId generatePlogId();

    String getPlogFileName(const PlogId& plogId) {
        return (m_plogPath + "/" + m_plogFileNamePrefix + String(plogId.id, PLOG_ID_LEN));
    }

    // the maximal size of plog file, default: PLOG_MAX_SIZE,  can be customized by setPlogMaxSize()
    uint32_t m_plogMaxSize;

    // the prefix of a plog file name, default: "plogid_", can be customized by setPlogFileNamePrefix()
    String m_plogFileNamePrefix;

    // the path to store plog files, default: "./plogData", can be initialized when an instance is created from the class
    String m_plogPath;

    // keep active plog ids and their information
    std::map<PlogId, PlogFileDescriptor, PlogIdComp> m_plogFileDescriptors;

    // uniform random engine to generate plog id.
    std::default_random_engine m_generator;
    std::uniform_int_distribution<int> m_distribution;

};  //  class PlogMock


}   //  namespace k2
