/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2018-2020 SChernykh   <https://github.com/SChernykh>
 * Copyright 2016-2020 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include "backend/common/Tags.h"
#include "backend/opencl/kernels/kawpow/KawPow_CalculateDAGKernel.h"
#include "backend/opencl/runners/OclKawPowRunner.h"
#include "backend/opencl/runners/tools/OclKawPow.h"
#include "backend/opencl/OclLaunchData.h"
#include "backend/opencl/wrappers/OclError.h"
#include "backend/opencl/wrappers/OclLib.h"
#include "base/io/log/Log.h"
#include "base/net/stratum/Job.h"

#include "base/io/log/Log.h"
#include "base/tools/Chrono.h"
#include "crypto/common/VirtualMemory.h"
#include "crypto/kawpow/KPHash.h"

#include "3rdparty/libethash/ethash_internal.h"
#include <Windows.h>


namespace xmrig {


OclKawPowRunner::OclKawPowRunner(size_t index, const OclLaunchData &data) : OclBaseRunner(index, data)
{
    if (data.device.vendorId() == OclVendor::OCL_VENDOR_NVIDIA) {
        m_options += " -DPLATFORM=OPENCL_PLATFORM_NVIDIA";
        m_workGroupSize = 32;
    }
}


OclKawPowRunner::~OclKawPowRunner()
{
    OclLib::release(m_lightCache);
    OclLib::release(m_dag);

    delete m_calculateDagKernel;

    OclLib::release(m_searchKernel);

    OclKawPow::clear();
}


void OclKawPowRunner::run(uint32_t nonce, uint32_t *hashOutput)
{
    const size_t local_work_size = 128;
    const size_t global_work_offset = nonce;
    const size_t global_work_size = m_intensity - (m_intensity % local_work_size);

    enqueueWriteBuffer(m_input, CL_FALSE, 0, 40, m_blob);

    const uint32_t zero = 0;
    enqueueWriteBuffer(m_output, CL_FALSE, 0, sizeof(uint32_t), &zero);

    const cl_int ret = OclLib::enqueueNDRangeKernel(m_queue, m_searchKernel, 1, &global_work_offset, &global_work_size, &local_work_size, 0, nullptr, nullptr);
    if (ret != CL_SUCCESS) {
        LOG_ERR("%s" RED(" error ") RED_BOLD("%s") RED(" when calling ") RED_BOLD("clEnqueueNDRangeKernel") RED(" for kernel ") RED_BOLD("progpow_search"),
            ocl_tag(), OclError::toString(ret));

        throw std::runtime_error(OclError::toString(ret));
    }

    uint32_t output[16] = {};
    enqueueReadBuffer(m_output, CL_TRUE, 0, sizeof(output), output);

    if (output[0] > 15) {
        output[0] = 15;
    }

    hashOutput[0xFF] = output[0];
    memcpy(hashOutput, output + 1, output[0] * sizeof(uint32_t));
}


void OclKawPowRunner::set(const Job &job, uint8_t *blob)
{
    m_blockHeight = static_cast<uint32_t>(job.height());
    m_searchProgram = OclKawPow::get(*this, m_blockHeight);
    m_searchKernel = OclLib::createKernel(m_searchProgram, "progpow_search");

    const uint32_t epoch = m_blockHeight / KPHash::EPOCH_LENGTH;

    const uint64_t dag_size = KPCache::dag_size(epoch);
    if (dag_size > m_dagCapacity) {
        OclLib::release(m_dag);

        m_dagCapacity = VirtualMemory::align(dag_size, 16 * 1024 * 1024);
        m_dag = OclLib::createBuffer(m_ctx, CL_MEM_READ_WRITE, m_dagCapacity);
    }

    if (epoch != m_epoch) {
        m_epoch = epoch;

        {
            std::lock_guard<std::mutex> lock(KPCache::s_cacheMutex);

            KPCache::s_cache.init(epoch);

            if (KPCache::s_cache.size() > m_lightCacheCapacity) {
                OclLib::release(m_lightCache);

                m_lightCacheCapacity = VirtualMemory::align(KPCache::s_cache.size());
                m_lightCache = OclLib::createBuffer(m_ctx, CL_MEM_READ_ONLY, m_lightCacheCapacity);
            }

            m_lightCacheSize = KPCache::s_cache.size();
            enqueueWriteBuffer(m_lightCache, CL_TRUE, 0, m_lightCacheSize, KPCache::s_cache.data());
        }

        const uint64_t start_ms = Chrono::steadyMSecs();

        const uint32_t dag_words = dag_size / sizeof(node);
        m_calculateDagKernel->setArgs(0, m_lightCache, m_dag, dag_words, m_lightCacheSize / sizeof(node));

        constexpr uint32_t N = 1 << 18;

        for (uint32_t start = 0; start < dag_words; start += N) {
            m_calculateDagKernel->setArg(0, sizeof(start), &start);
            m_calculateDagKernel->enqueue(m_queue, N, m_workGroupSize);
        }

        OclLib::finish(m_queue);

        LOG_INFO("KawPow DAG for epoch %u calculated (%" PRIu64 " ms)", epoch, Chrono::steadyMSecs() - start_ms);
    }

    const uint64_t target = job.target();
    const uint32_t hack_false = 0;

    OclLib::setKernelArg(m_searchKernel, 0, sizeof(m_dag), &m_dag);
    OclLib::setKernelArg(m_searchKernel, 1, sizeof(m_input), &m_input);
    OclLib::setKernelArg(m_searchKernel, 2, sizeof(target), &target);
    OclLib::setKernelArg(m_searchKernel, 3, sizeof(hack_false), &hack_false);
    OclLib::setKernelArg(m_searchKernel, 4, sizeof(m_output), &m_output);

    m_blob = blob;
    enqueueWriteBuffer(m_input, CL_TRUE, 0, sizeof(m_blob), m_blob);
}


void xmrig::OclKawPowRunner::build()
{
    OclBaseRunner::build();

    m_calculateDagKernel = new KawPow_CalculateDAGKernel(m_program);
}


void xmrig::OclKawPowRunner::init()
{
    OclBaseRunner::init();
}

} // namespace xmrig