#include "FrameGen/SourceCameraSelection.h"
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

struct Node;
struct Object
{
    virtual ~Object() = default;
    virtual Node* AsNode() { return nullptr; }
    Object* parent{};  // Deliberately unsafe in the stale-parent regression.
    bool cameraType{true};
};
using Pointer = std::shared_ptr<Object>;
struct Node : Object
{
    Node* AsNode() override { return this; }
    const auto& GetChildren() const { return children; }
    std::vector<Pointer> children;
};
struct Entry { Object* pReferenceCamera{}; bool UseJitter{}; int matrixTag{}; };
using TheosRenderPipeline::SourceDLSSG::SelectOwnedCameraView;
constexpr auto IsCamera = [](const Object* object) { return object->cameraType; };

int main()
{
    // Any accidental cache-pointer dereference is a real access violation,
    // including when no live view matches. No runtime/game DLLs are loaded.
    auto* unreadable = static_cast<Object*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    Require(unreadable != nullptr, "allocate unreadable stale-camera page");
    auto root = std::make_shared<Node>();
    auto branch = std::make_shared<Node>();
    auto camera = std::make_shared<Object>();
    auto retired = std::make_shared<Object>();
    retired->parent = unreadable;
    branch->children = {nullptr, camera};
    root->children = {nullptr, branch};
    std::vector<Entry> cache{{unreadable, true, 1}, {retired.get(), true, 2},
        {camera.get(), false, 3}, {camera.get(), true, 4}, {nullptr, true, 5}};
    auto select = [&](bool jittered) { return SelectOwnedCameraView(Pointer(root), cache, jittered, IsCamera); };

    auto selected = select(true);
    Require(selected.entry == &cache[3] && selected.camera == camera && !selected.ambiguous,
        "ignore unreadable/retired entries and select nested live jittered view");
    selected = select(false);
    Require(selected.entry == &cache[2], "preserve unjittered matrix selection");
    auto geometry = std::make_shared<Object>();
    geometry->cameraType = false;
    root->children.push_back(geometry);
    cache.push_back({geometry.get(), true, 8});
    selected = select(true);
    Require(selected.camera == camera && !selected.ambiguous, "ignore cached address reused by a live non-camera");
    root->children.pop_back();
    cache.pop_back();
    cache.push_back({camera.get(), true, 6});
    selected = select(true);
    Require(selected.ambiguous && !selected.entry && !selected.camera, "reject duplicate matching views");
    cache.pop_back();
    root->children.push_back(retired);
    selected = select(true);
    Require(selected.ambiguous, "reject two distinct live player cameras");
    root->children.pop_back();

    // The result must retain the object after it is detached from the scene.
    auto replacement = std::make_shared<Object>();
    selected = select(true);
    std::weak_ptr<Object> retained = camera;
    branch->children.clear();
    camera.reset();
    Require(!retained.expired() && selected.camera.get() == cache[3].pReferenceCamera,
        "selection owns camera throughout constant capture");
    selected = {};
    Require(retained.expired(), "release camera after capture");
    selected = select(true);
    Require(!selected.entry && !selected.ambiguous, "detached and unreadable cache entries cannot select a camera");

    branch->children = {replacement};
    cache.push_back({replacement.get(), true, 7});
    selected = select(true);
    Require(selected.camera == replacement && selected.entry->matrixTag == 7, "select replacement camera after transition");
    Require(!select(false).entry, "do not substitute wrong jitter variant");
    Require(!SelectOwnedCameraView(Pointer{}, cache, true, IsCamera).entry, "missing root has no view");

    // Match the old eight-parent limit, including a camera exactly at it.
    auto cursor = root;
    root->children.clear();
    for (unsigned i = 0; i < 7; ++i) {
        auto next = std::make_shared<Node>();
        cursor->children = {next};
        cursor = next;
    }
    cursor->children = {replacement};
    Require(select(true).camera == replacement, "camera eight edges below root is eligible");
    auto ninth = std::make_shared<Node>();
    ninth->children = {replacement};
    cursor->children = {ninth};
    Require(!select(true).entry, "camera beyond ancestry limit is excluded");
    root->children = {root};
    Require(!select(true).entry, "bounded traversal terminates for cyclic tree");
    root->children.clear();
    Require(VirtualFree(unreadable, 0, MEM_RELEASE) != 0, "release guard page");
    std::puts("Source camera selection passed");
}
