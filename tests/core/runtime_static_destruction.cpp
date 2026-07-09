#include <saccade/saccade.h>

#include <cstdlib>

namespace {

SaccadeRuntimeHandle runtime = 0;

struct RuntimeOwner {
    ~RuntimeOwner() {
        if (runtime != 0 && saccade_runtime_destroy(runtime) != SACCADE_OK) {
            std::abort();
        }
    }
};

RuntimeOwner owner;

}  // namespace

int main() {
    SaccadeRuntimeDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    return saccade_runtime_create(&desc, &runtime) == SACCADE_OK ? 0 : 1;
}
