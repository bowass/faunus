#pragma once

#include <random>
#include <cmath>
#include <algorithm>
#include <limits>
#include "../config/config.hpp"

namespace util {

/**
 * Zipfian (skewed) distribution sampler implementing the algorithm from
 * "Quickly Generating Billion-Record Synthetic Databases" by Gray et al.
 */
class ZipfianSampler {
public:
    explicit ZipfianSampler(double theta)
        : theta_(theta), alpha_(theta_ == 0.0 ? 0.0 : 1.0 / (1.0 - theta_)),
          zeta_2_theta_(harmonic(2, theta_)) {
        reset();
    }

    void reset() {
        items_ = 0;
        zetan_ = 0.0;
        eta_ = 0.0;
    }

    void update_item_count(size_t count) {
        if (count == items_) return;

        if (count == 0) {
            reset();
            return;
        }

        if (items_ == 0) {
            items_ = 1;
            zetan_ = 1.0;
        }

        if (count > items_) {
            for (size_t i = items_ + 1; i <= count; ++i) {
                zetan_ += std::pow(static_cast<double>(i), -theta_);
            }
        } else {
            for (size_t i = items_; i > count; --i) {
                zetan_ -= std::pow(static_cast<double>(i), -theta_);
            }
        }

        items_ = count;
        if (items_ == 0) {
            zetan_ = 0.0;
            eta_ = 0.0;
            return;
        }

        if (theta_ == 0.0) {
            eta_ = 0.0;
            return;
        }

        const double one_minus_theta = 1.0 - theta_;
        const double denom_term = (items_ > 1)
            ? (1.0 - std::pow(2.0 / static_cast<double>(items_), one_minus_theta))
            : 1.0;

        double numerator = 1.0 - zeta_2_theta_ / zetan_;
        if (numerator <= 0.0) {
            numerator = std::numeric_limits<double>::min();
        }
        eta_ = denom_term / numerator;
    }

    template <typename RNG>
    size_t sample(RNG& rng) const {
        if (items_ == 0) return 0;
        if (theta_ == 0.0 || items_ == 1) {
            return 0;
        }

        std::uniform_real_distribution<double> dist(0.0, 1.0);
        double u = dist(rng);
        double uz = u * zetan_;

        if (uz < 1.0) return 0;
        if (uz < 1.0 + std::pow(0.5, theta_)) return std::min<size_t>(1, items_ - 1);

        size_t idx = static_cast<size_t>(std::floor(items_ * std::pow(eta_ * u - eta_ + 1.0, alpha_)));
        if (idx >= items_) idx = items_ - 1;
        return idx;
    }

private:
    static double harmonic(size_t n, double theta) {
        if (n == 0) return 0.0;
        double sum = 0.0;
        for (size_t i = 1; i <= n; ++i) {
            sum += std::pow(static_cast<double>(i), -theta);
        }
        return sum;
    }

    double theta_;
    double alpha_;
    double zeta_2_theta_;
    size_t items_ = 0;
    double zetan_ = 0.0;
    double eta_ = 0.0;
};

/**
 * Uniform distribution sampler for consistent interface with ZipfianSampler.
 */
class UniformSampler {
public:
    UniformSampler() = default;

    void reset() {
        active_count_ = 0;
    }

    void update_item_count(size_t count) {
        active_count_ = count;
    }

    template <typename RNG>
    size_t sample(RNG& rng) const {
        if (active_count_ == 0) return 0;
        std::uniform_int_distribution<size_t> dist(0, active_count_ - 1);
        return dist(rng);
    }

private:
    size_t active_count_ = 0;
};

/**
 * Key selection sampler that wraps either uniform or zipfian distribution
 * based on configuration.
 */
class KeySelectionSampler {
public:
    explicit KeySelectionSampler(const DistributionConfig& config)
        : type_(config.type), zipf_sampler_(config.zipf), uniform_sampler_() {}

    void update_active_count(size_t count) {
        active_count_ = count;
        if (type_ == DistributionConfig::Type::Skewed) {
            zipf_sampler_.update_item_count(count);
        } else {
            uniform_sampler_.update_item_count(count);
        }
    }

    template <typename RNG>
    size_t sample(RNG& rng) const {
        if (active_count_ == 0) return 0;
        
        size_t idx;
        if (type_ == DistributionConfig::Type::Uniform) {
            idx = uniform_sampler_.sample(rng);
        } else {
            idx = zipf_sampler_.sample(rng);
        }
        
        if (idx >= active_count_) idx = active_count_ - 1;
        return idx;
    }

private:
    DistributionConfig::Type type_;
    size_t active_count_ = 0;
    ZipfianSampler zipf_sampler_;
    UniformSampler uniform_sampler_;
};

} // namespace util