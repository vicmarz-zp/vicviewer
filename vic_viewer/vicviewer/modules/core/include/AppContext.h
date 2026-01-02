#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <typeinfo>

namespace vic {

class IService {
public:
    virtual ~IService() = default;
};

class AppContext {
public:
    template <typename T>
    void registerService(std::shared_ptr<T> service) {
        std::lock_guard lock(mutex_);
        services_[typeid(T).name()] = std::static_pointer_cast<IService>(service);
    }

    template <typename T>
    std::shared_ptr<T> getService() const {
        std::lock_guard lock(mutex_);
        auto it = services_.find(typeid(T).name());
        if (it == services_.end()) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(it->second);
    }

private:
    mutable std::mutex mutex_{};
    std::unordered_map<std::string, std::shared_ptr<IService>> services_{};
};

} // namespace vic
