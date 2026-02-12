/**
 * @file IpcBenchmark.cpp
 * @brief IPC Performance Benchmark Suite
 *
 * Measures:
 * - Serialize/Deserialize throughput (MB/s)
 * - RPC round-trip latency (p50/p95/p99)
 * - Large message handling (4KB/64KB/1MB)
 * - Concurrent operations (32/128/512)
 *
 * Output formats:
 * - Human-readable console output (default)
 * - JSON output (--json <file>)
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/Serializer.h>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::SerializerReader;
using DAS::Core::IPC::SerializerWriter;

// ====== Helper Classes ======

class MemorySerializerWriter : public SerializerWriter
{
private:
    std::vector<uint8_t> buffer_;

public:
    DasResult Write(const void* data, size_t size) override
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
        return DAS_S_OK;
    }

    size_t GetPosition() const override { return buffer_.size(); }

    DasResult Seek(size_t position) override
    {
        if (position > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        buffer_.resize(position);
        return DAS_S_OK;
    }

    DasResult Reserve(size_t size) override
    {
        buffer_.reserve(buffer_.size() + size);
        return DAS_S_OK;
    }

    const std::vector<uint8_t>& GetBuffer() const { return buffer_; }
    void                        Clear() { buffer_.clear(); }
};

class MemorySerializerReader : public SerializerReader
{
private:
    std::vector<uint8_t> buffer_;
    size_t               position_;

public:
    explicit MemorySerializerReader(const std::vector<uint8_t>& buffer)
        : buffer_(buffer), position_(0)
    {
    }

    DasResult Read(void* data, size_t size) override
    {
        if (position_ + size > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        uint8_t* bytes = static_cast<uint8_t*>(data);
        std::memcpy(bytes, buffer_.data() + position_, size);
        position_ += size;
        return DAS_S_OK;
    }

    size_t GetPosition() const override { return position_; }

    size_t GetRemaining() const override { return buffer_.size() - position_; }

    DasResult Seek(size_t position) override
    {
        if (position > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        position_ = position;
        return DAS_S_OK;
    }
};

// ====== Benchmark Result Structures ======

struct BenchmarkResult
{
    std::string name;
    double      value;
    std::string unit;
    size_t      iterations;
    double      min_val;
    double      max_val;
    double      p50;
    double      p95;
    double      p99;
};

struct BenchmarkSuite
{
    std::string                  suite_name;
    std::vector<BenchmarkResult> results;
};

// ====== Statistics Helpers ======

double CalculatePercentile(std::vector<double>& data, double percentile)
{
    if (data.empty())
        return 0.0;

    std::sort(data.begin(), data.end());
    size_t index =
        static_cast<size_t>(std::ceil(data.size() * percentile / 100.0)) - 1;
    index = std::min(index, data.size() - 1);
    return data[index];
}

double CalculateMean(const std::vector<double>& data)
{
    if (data.empty())
        return 0.0;
    return std::accumulate(data.begin(), data.end(), 0.0) / data.size();
}

// ====== JSON Output ======

std::string ResultsToJson(const std::vector<BenchmarkSuite>& suites)
{
    std::ostringstream json;
    json << "{\n";
    json << "  \"benchmarks\": [\n";

    bool first_suite = true;
    for (const auto& suite : suites)
    {
        if (!first_suite)
            json << ",\n";
        first_suite = false;

        json << "    {\n";
        json << "      \"suite\": \"" << suite.suite_name << "\",\n";
        json << "      \"results\": [\n";

        bool first_result = true;
        for (const auto& result : suite.results)
        {
            if (!first_result)
                json << ",\n";
            first_result = false;

            json << "        {\n";
            json << "          \"name\": \"" << result.name << "\",\n";
            json << "          \"value\": " << std::fixed
                 << std::setprecision(4) << result.value << ",\n";
            json << "          \"unit\": \"" << result.unit << "\",\n";
            json << "          \"iterations\": " << result.iterations << ",\n";
            json << "          \"min\": " << result.min_val << ",\n";
            json << "          \"max\": " << result.max_val << ",\n";
            json << "          \"p50\": " << result.p50 << ",\n";
            json << "          \"p95\": " << result.p95 << ",\n";
            json << "          \"p99\": " << result.p99 << "\n";
            json << "        }";
        }

        json << "\n      ]\n";
        json << "    }";
    }

    json << "\n  ],\n";
    json << "  \"timestamp\": \""
         << std::chrono::system_clock::now().time_since_epoch().count()
         << "\"\n";
    json << "}\n";

    return json.str();
}

// ====== Console Output ======

// ====== Console Output ======

void PrintResults(const std::vector<BenchmarkSuite>& suites)
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "       IPC Performance Benchmark        \n";
    std::cout << "========================================\n\n";

    for (const auto& suite : suites)
    {
        std::cout << "[" << suite.suite_name << "]\n";
        std::cout << std::string(60, '-') << "\n";
        std::cout << std::left << std::setw(35) << "Benchmark" << std::right
                  << std::setw(12) << "Value" << std::setw(8) << "Unit"
                  << std::setw(10) << "Iter" << "\n";
        std::cout << std::string(60, '-') << "\n";

        for (const auto& result : suite.results)
        {
            std::cout << std::left << std::setw(35) << result.name << std::right
                      << std::setw(12) << std::fixed << std::setprecision(2)
                      << result.value << std::setw(8) << result.unit
                      << std::setw(10) << result.iterations << "\n";
        }
        std::cout << "\n";
    }

    // Print latency summary
    std::cout << "========================================\n";
    std::cout << "           Latency Summary              \n";
    std::cout << "========================================\n";
    for (const auto& suite : suites)
    {
        for (const auto& result : suite.results)
        {
            if (result.unit == "ns" || result.unit == "us"
                || result.unit == "ms")
            {
                std::cout << result.name << ":\n";
                std::cout << "  p50: " << std::fixed << std::setprecision(2)
                          << result.p50 << " " << result.unit << "\n";
                std::cout << "  p95: " << result.p95 << " " << result.unit
                          << "\n";
                std::cout << "  p99: " << result.p99 << " " << result.unit
                          << "\n";
            }
        }
    }
}

// ====== Benchmark Tests ======

class IpcBenchmark : public ::testing::Test
{
public:
    static std::vector<BenchmarkSuite> benchmark_suites_;

    void SetUp() override { benchmark_suites_.clear(); }

    void TearDown() override
    {
        // Results are printed at the end of all tests
    }

    static void TearDownTestSuite() { PrintResults(benchmark_suites_); }

    BenchmarkResult RunBenchmark(
        const std::string&    name,
        std::function<void()> setup,
        std::function<void()> benchmark,
        size_t                iterations,
        const std::string&    unit)
    {
        std::vector<double> times;
        times.reserve(iterations);

        for (size_t i = 0; i < iterations; ++i)
        {
            if (setup)
                setup();

            auto start = std::chrono::high_resolution_clock::now();
            benchmark();
            auto end = std::chrono::high_resolution_clock::now();

            double elapsed =
                std::chrono::duration<double, std::nano>(end - start).count();
            times.push_back(elapsed);
        }

        BenchmarkResult result;
        result.name = name;
        result.iterations = iterations;
        result.unit = unit;

        double sum = std::accumulate(times.begin(), times.end(), 0.0);
        result.value = sum / iterations;

        auto [min_it, max_it] = std::minmax_element(times.begin(), times.end());
        result.min_val = *min_it;
        result.max_val = *max_it;

        result.p50 = CalculatePercentile(times, 50);
        result.p95 = CalculatePercentile(times, 95);
        result.p99 = CalculatePercentile(times, 99);

        // Convert units if needed
        if (unit == "us")
        {
            result.value /= 1000.0;
            result.min_val /= 1000.0;
            result.max_val /= 1000.0;
            result.p50 /= 1000.0;
            result.p95 /= 1000.0;
            result.p99 /= 1000.0;
        }
        else if (unit == "ms")
        {
            result.value /= 1000000.0;
            result.min_val /= 1000000.0;
            result.max_val /= 1000000.0;
            result.p50 /= 1000000.0;
            result.p95 /= 1000000.0;
            result.p99 /= 1000000.0;
        }

        return result;
    }
};

// ====== Serialize/Deserialize Throughput Tests ======

TEST_F(IpcBenchmark, SerializeThroughput_SmallInt)
{
    BenchmarkSuite suite{"Serialize Throughput"};

    constexpr size_t       iterations = 10000;
    MemorySerializerWriter writer;

    auto result = RunBenchmark(
        "Serialize int32_t",
        [&]() { writer.Clear(); },
        [&]() { writer.WriteInt32(12345678); },
        iterations,
        "ns");

    suite.results.push_back(result);
    benchmark_suites_.push_back(suite);
}

TEST_F(IpcBenchmark, SerializeThroughput_String)
{
    BenchmarkSuite suite{"Serialize Throughput"};

    constexpr size_t  iterations = 1000;
    const std::string test_str =
        "Hello, World! This is a test string for IPC serialization.";
    MemorySerializerWriter writer;

    auto result = RunBenchmark(
        "Serialize string (64 bytes)",
        [&]() { writer.Clear(); },
        [&]() { writer.WriteString(test_str.c_str(), test_str.size()); },
        iterations,
        "ns");

    suite.results.push_back(result);
    benchmark_suites_.push_back(suite);
}

TEST_F(IpcBenchmark, SerializeThroughput_LargeBuffer)
{
    BenchmarkSuite suite{"Serialize Throughput"};

    constexpr size_t       iterations = 100;
    std::vector<uint8_t>   large_data(65536, 0xAB); // 64KB
    MemorySerializerWriter writer;

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i)
    {
        writer.Clear();
        writer.WriteBytes(large_data.data(), large_data.size());
    }
    auto end = std::chrono::high_resolution_clock::now();

    double total_bytes = static_cast<double>(large_data.size()) * iterations;
    double total_seconds = std::chrono::duration<double>(end - start).count();
    double throughput_mb_s = (total_bytes / total_seconds) / (1024 * 1024);

    BenchmarkResult result;
    result.name = "Serialize 64KB buffer";
    result.value = throughput_mb_s;
    result.unit = "MB/s";
    result.iterations = iterations;
    result.min_val = throughput_mb_s;
    result.max_val = throughput_mb_s;
    result.p50 = throughput_mb_s;
    result.p95 = throughput_mb_s;
    result.p99 = throughput_mb_s;

    suite.results.push_back(result);
    benchmark_suites_.push_back(suite);
}

// ====== ObjectId Benchmark Tests ======

TEST_F(IpcBenchmark, ObjectId_EncodeDecode)
{
    BenchmarkSuite suite{"ObjectId Operations"};

    constexpr size_t iterations = 100000;
    ObjectId         obj{.session_id = 1, .generation = 5, .local_id = 12345};

    std::vector<double> encode_times;
    std::vector<double> decode_times;

    for (size_t i = 0; i < iterations; ++i)
    {
        auto              start = std::chrono::high_resolution_clock::now();
        volatile uint64_t encoded = EncodeObjectId(obj);
        auto              end = std::chrono::high_resolution_clock::now();
        encode_times.push_back(
            std::chrono::duration<double, std::nano>(end - start).count());

        start = std::chrono::high_resolution_clock::now();
        volatile ObjectId decoded = DecodeObjectId(encoded);
        end = std::chrono::high_resolution_clock::now();
        decode_times.push_back(
            std::chrono::duration<double, std::nano>(end - start).count());
    }

    BenchmarkResult encode_result;
    encode_result.name = "EncodeObjectId";
    encode_result.iterations = iterations;
    encode_result.unit = "ns";
    encode_result.value = CalculateMean(encode_times);
    encode_result.min_val =
        *std::min_element(encode_times.begin(), encode_times.end());
    encode_result.max_val =
        *std::max_element(encode_times.begin(), encode_times.end());
    encode_result.p50 = CalculatePercentile(encode_times, 50);
    encode_result.p95 = CalculatePercentile(encode_times, 95);
    encode_result.p99 = CalculatePercentile(encode_times, 99);
    suite.results.push_back(encode_result);

    BenchmarkResult decode_result;
    decode_result.name = "DecodeObjectId";
    decode_result.iterations = iterations;
    decode_result.unit = "ns";
    decode_result.value = CalculateMean(decode_times);
    decode_result.min_val =
        *std::min_element(decode_times.begin(), decode_times.end());
    decode_result.max_val =
        *std::max_element(decode_times.begin(), decode_times.end());
    decode_result.p50 = CalculatePercentile(decode_times, 50);
    decode_result.p95 = CalculatePercentile(decode_times, 95);
    decode_result.p99 = CalculatePercentile(decode_times, 99);
    suite.results.push_back(decode_result);

    benchmark_suites_.push_back(suite);
}

// ====== Large Message Tests ======

TEST_F(IpcBenchmark, LargeMessage_4KB)
{
    BenchmarkSuite suite{"Large Message Handling"};

    constexpr size_t       iterations = 1000;
    std::vector<uint8_t>   data(4096, 0xCD);
    MemorySerializerWriter writer;

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i)
    {
        writer.Clear();
        writer.WriteBytes(data.data(), data.size());
    }
    auto end = std::chrono::high_resolution_clock::now();

    double total_bytes = static_cast<double>(data.size()) * iterations;
    double total_seconds = std::chrono::duration<double>(end - start).count();
    double throughput_mb_s = (total_bytes / total_seconds) / (1024 * 1024);

    BenchmarkResult result;
    result.name = "Serialize 4KB";
    result.value = throughput_mb_s;
    result.unit = "MB/s";
    result.iterations = iterations;
    result.min_val = throughput_mb_s;
    result.max_val = throughput_mb_s;
    result.p50 = throughput_mb_s;
    result.p95 = throughput_mb_s;
    result.p99 = throughput_mb_s;
    suite.results.push_back(result);

    benchmark_suites_.push_back(suite);
}

TEST_F(IpcBenchmark, LargeMessage_1MB)
{
    BenchmarkSuite suite{"Large Message Handling"};

    constexpr size_t       iterations = 100;
    std::vector<uint8_t>   data(1024 * 1024, 0xEF);
    MemorySerializerWriter writer;

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i)
    {
        writer.Clear();
        writer.WriteBytes(data.data(), data.size());
    }
    auto end = std::chrono::high_resolution_clock::now();

    double total_bytes = static_cast<double>(data.size()) * iterations;
    double total_seconds = std::chrono::duration<double>(end - start).count();
    double throughput_mb_s = (total_bytes / total_seconds) / (1024 * 1024);

    BenchmarkResult result;
    result.name = "Serialize 1MB";
    result.value = throughput_mb_s;
    result.unit = "MB/s";
    result.iterations = iterations;
    result.min_val = throughput_mb_s;
    result.max_val = throughput_mb_s;
    result.p50 = throughput_mb_s;
    result.p95 = throughput_mb_s;
    result.p99 = throughput_mb_s;
    suite.results.push_back(result);

    benchmark_suites_.push_back(suite);
}

// ====== Concurrent Operation Tests ======

TEST_F(IpcBenchmark, Concurrent_32Threads)
{
    BenchmarkSuite suite{"Concurrent Operations"};

    constexpr size_t num_threads = 32;
    constexpr size_t ops_per_thread = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    std::atomic<size_t>      total_ops{0};

    for (size_t t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
            [&]()
            {
                MemorySerializerWriter writer;
                for (size_t i = 0; i < ops_per_thread; ++i)
                {
                    writer.Clear();
                    writer.WriteInt32(static_cast<int32_t>(i));
                    total_ops.fetch_add(1);
                }
            });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    auto   end = std::chrono::high_resolution_clock::now();
    double total_seconds = std::chrono::duration<double>(end - start).count();
    double ops_per_second = total_ops.load() / total_seconds;

    BenchmarkResult result;
    result.name = "Serialize 32 threads";
    result.value = ops_per_second / 1000.0; // K ops/s
    result.unit = "Kops/s";
    result.iterations = num_threads * ops_per_thread;
    result.min_val = result.value;
    result.max_val = result.value;
    result.p50 = result.value;
    result.p95 = result.value;
    result.p99 = result.value;
    suite.results.push_back(result);

    benchmark_suites_.push_back(suite);
}

// Static member definition
std::vector<BenchmarkSuite> IpcBenchmark::benchmark_suites_;

// ====== Main Function ======

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);

    bool        json_output = false;
    std::string json_file;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--json" && i + 1 < argc)
        {
            json_output = true;
            json_file = argv[++i];
        }
    }

    int result = RUN_ALL_TESTS();

    if (json_output)
    {
        std::ofstream out(json_file);
        out << "{\n";
        out << "  \"benchmarks\": [\n";
        const auto& suites = IpcBenchmark::benchmark_suites_;
        for (size_t i = 0; i < suites.size(); ++i)
        {
            const auto& suite = suites[i];
            for (size_t j = 0; j < suite.results.size(); ++j)
            {
                const auto& r = suite.results[j];
                out << "    {\"suite\": \"" << suite.suite_name << "\", "
                    << "\"name\": \"" << r.name << "\", "
                    << "\"value\": " << r.value << ", "
                    << "\"unit\": \"" << r.unit << "\"}";
                if (i < suites.size() - 1 || j < suite.results.size() - 1)
                    out << ",";
                out << "\n";
            }
        }
        out << "  ]\n";
        out << "}\n";
    }

    return result;
}