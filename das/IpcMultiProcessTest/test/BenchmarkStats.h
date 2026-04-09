/**
 * @file BenchmarkStats.h
 * @brief Benchmark statistics utilities for IPC performance measurement.
 *
 * Provides:
 * - BenchmarkResult / BenchmarkSuite data structures
 * - CalculatePercentile, CalculateMean statistics helpers
 * - RunBenchmark generic benchmark runner
 * - PrintResults console output
 */

#ifndef DAS_IPC_PERFORMANCE_BENCHMARK_STATS_H
#define DAS_IPC_PERFORMANCE_BENCHMARK_STATS_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace das
{
    namespace benchmark
    {

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

        inline double CalculatePercentile(
            std::vector<double>& data,
            double               percentile)
        {
            if (data.empty())
            {
                return 0.0;
            }

            std::sort(data.begin(), data.end());
            size_t index =
                static_cast<size_t>(std::ceil(data.size() * percentile / 100.0))
                - 1;
            index = std::min(index, data.size() - 1);
            return data[index];
        }

        inline double CalculateMean(const std::vector<double>& data)
        {
            if (data.empty())
            {
                return 0.0;
            }
            return std::accumulate(data.begin(), data.end(), 0.0) / data.size();
        }

        // ====== Benchmark Runner ======

        inline BenchmarkResult RunBenchmark(
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
                {
                    setup();
                }

                auto start = std::chrono::steady_clock::now();
                benchmark();
                auto end = std::chrono::steady_clock::now();

                double elapsed =
                    std::chrono::duration<double, std::nano>(end - start)
                        .count();
                times.push_back(elapsed);
            }

            BenchmarkResult result;
            result.name = name;
            result.iterations = iterations;
            result.unit = unit;

            double sum = std::accumulate(times.begin(), times.end(), 0.0);
            result.value = sum / static_cast<double>(iterations);

            auto [min_it, max_it] =
                std::minmax_element(times.begin(), times.end());
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

        // ====== Console Output ======

        inline void PrintResults(const std::vector<BenchmarkSuite>& suites)
        {
            std::cout << "\n";
            std::cout << "========================================\n";
            std::cout << "       IPC Performance Benchmark        \n";
            std::cout << "========================================\n\n";

            for (const auto& suite : suites)
            {
                std::cout << "[" << suite.suite_name << "]\n";
                std::cout << std::string(60, '-') << "\n";
                std::cout << std::left << std::setw(35) << "Benchmark"
                          << std::right << std::setw(12) << "Value"
                          << std::setw(8) << "Unit" << std::setw(10) << "Iter"
                          << "\n";
                std::cout << std::string(60, '-') << "\n";

                for (const auto& result : suite.results)
                {
                    std::cout << std::left << std::setw(35) << result.name
                              << std::right << std::setw(12) << std::fixed
                              << std::setprecision(2) << result.value
                              << std::setw(8) << result.unit << std::setw(10)
                              << result.iterations << "\n";
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
                        std::cout << "  p50: " << std::fixed
                                  << std::setprecision(2) << result.p50 << " "
                                  << result.unit << "\n";
                        std::cout << "  p95: " << result.p95 << " "
                                  << result.unit << "\n";
                        std::cout << "  p99: " << result.p99 << " "
                                  << result.unit << "\n";
                    }
                }
            }
        }

    } // namespace benchmark
} // namespace das

#endif // DAS_IPC_PERFORMANCE_BENCHMARK_STATS_H
