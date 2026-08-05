#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

// All database records live in this file.  The program deliberately keeps no
// in-memory index: each lookup reads the required hash slots and tree nodes.
namespace {

constexpr const char *DB_NAME = "file_storage.db";
constexpr std::uint32_t TABLE_SIZE = 262144; // > 2 * maximum live key count
constexpr std::int32_t MAX_NODES = 100000;
constexpr std::int32_t NIL = -1;
constexpr std::uint64_t MAGIC = 0x4653544f52453131ULL; // "FSTORE11"

#pragma pack(push, 1)
struct Header {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t tableSize;
    std::int32_t nodeCount;
    std::int32_t freeHead;
};

struct Slot {
    std::uint8_t state; // 0 empty, 1 used, 2 tombstone
    char key[65];
    std::int32_t root;
};

struct Node {
    std::int32_t left;
    std::int32_t right;
    std::int32_t value;
    std::int32_t height;
};
#pragma pack(pop)

class Database {
public:
    Database() {
        fd_ = open(DB_NAME, O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) std::exit(1);
        Header disk{};
        const ssize_t got = pread(fd_, &disk, sizeof(disk), 0);
        if (got != static_cast<ssize_t>(sizeof(disk)) ||
            disk.magic != MAGIC || disk.version != 1 || disk.tableSize != TABLE_SIZE) {
            initialize();
        } else {
            header_ = disk;
        }
        // The tree area is a bounded, shared view of the database file.  It
        // avoids millions of tiny system calls while remaining file-backed;
        // the hash index is still accessed one slot at a time.
        if (ftruncate(fd_, databaseSize()) != 0) std::exit(1);
        nodeMapOffset_ = nodesOffset() & ~static_cast<off_t>(4095);
        nodeMapLength_ = static_cast<std::size_t>(nodesOffset() - nodeMapOffset_) +
                         static_cast<std::size_t>(MAX_NODES) * sizeof(Node);
        nodeMap_ = static_cast<char *>(mmap(nullptr, nodeMapLength_, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd_, nodeMapOffset_));
        if (nodeMap_ == MAP_FAILED) std::exit(1);
    }

    ~Database() {
        msync(nodeMap_, nodeMapLength_, MS_ASYNC);
        munmap(nodeMap_, nodeMapLength_);
        close(fd_);
    }

    void insert(const std::string &key, std::int32_t value) {
        const std::uint32_t pos = locate(key, true);
        Slot slot = readSlot(pos);
        if (slot.state != 1) {
            slot = Slot{};
            slot.state = 1;
            std::memcpy(slot.key, key.c_str(), key.size());
            slot.root = NIL;
        }
        bool inserted = false;
        slot.root = insertNode(slot.root, value, inserted);
        if (inserted) writeSlot(pos, slot);
    }

    void erase(const std::string &key, std::int32_t value) {
        const std::uint32_t pos = locate(key, false);
        if (pos == TABLE_SIZE) return;
        Slot slot = readSlot(pos);
        bool removed = false;
        slot.root = eraseNode(slot.root, value, removed);
        if (!removed) return;
        if (slot.root == NIL) {
            // Tombstones retain probe-chain correctness without retaining an
            // index that has no values.
            slot = Slot{};
            slot.state = 2;
        }
        writeSlot(pos, slot);
    }

    void find(const std::string &key) {
        const std::uint32_t pos = locate(key, false);
        if (pos == TABLE_SIZE) {
            std::cout << "null\n";
            return;
        }
        const Slot slot = readSlot(pos);
        bool first = true;
        printInOrder(slot.root, first);
        if (first) std::cout << "null";
        std::cout << '\n';
    }

private:
    int fd_;
    Header header_{};
    char *nodeMap_ = nullptr;
    off_t nodeMapOffset_ = 0;
    std::size_t nodeMapLength_ = 0;

    static constexpr off_t slotsOffset() { return static_cast<off_t>(sizeof(Header)); }
    static constexpr off_t nodesOffset() {
        return slotsOffset() + static_cast<off_t>(TABLE_SIZE) * sizeof(Slot);
    }
    static off_t slotOffset(std::uint32_t pos) {
        return slotsOffset() + static_cast<off_t>(pos) * sizeof(Slot);
    }
    static off_t nodeOffset(std::int32_t index) {
        return nodesOffset() + static_cast<off_t>(index) * sizeof(Node);
    }
    static off_t databaseSize() {
        return nodesOffset() + static_cast<off_t>(MAX_NODES) * sizeof(Node);
    }

    void initialize() {
        header_.magic = MAGIC;
        header_.version = 1;
        header_.tableSize = TABLE_SIZE;
        header_.nodeCount = 0;
        header_.freeHead = NIL;
        // A sparse extension gives all hash slots their required zero state
        // without allocating or retaining a large initialization buffer.
        if (ftruncate(fd_, databaseSize()) != 0) std::exit(1);
        writeHeader();
    }

    void writeHeader() const {
        if (pwrite(fd_, &header_, sizeof(header_), 0) != static_cast<ssize_t>(sizeof(header_)))
            std::exit(1);
    }

    Slot readSlot(std::uint32_t pos) const {
        Slot slot{};
        if (pread(fd_, &slot, sizeof(slot), slotOffset(pos)) != static_cast<ssize_t>(sizeof(slot)))
            std::exit(1);
        return slot;
    }
    void writeSlot(std::uint32_t pos, const Slot &slot) const {
        if (pwrite(fd_, &slot, sizeof(slot), slotOffset(pos)) != static_cast<ssize_t>(sizeof(slot)))
            std::exit(1);
    }
    Node readNode(std::int32_t index) const {
        Node node{};
        std::memcpy(&node, nodeMap_ + (nodeOffset(index) - nodeMapOffset_), sizeof(node));
        return node;
    }
    void writeNode(std::int32_t index, const Node &node) const {
        std::memcpy(nodeMap_ + (nodeOffset(index) - nodeMapOffset_), &node, sizeof(node));
    }

    static std::uint64_t hash(const std::string &s) {
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    static bool sameKey(const Slot &slot, const std::string &key) {
        return std::strncmp(slot.key, key.c_str(), 65) == 0;
    }

    // When create is true, return an existing slot or a reusable tombstone.
    std::uint32_t locate(const std::string &key, bool create) const {
        std::uint32_t pos = static_cast<std::uint32_t>(hash(key) & (TABLE_SIZE - 1));
        std::uint32_t firstTombstone = TABLE_SIZE;
        for (std::uint32_t examined = 0; examined < TABLE_SIZE; ++examined) {
            const Slot slot = readSlot(pos);
            if (slot.state == 0) {
                if (!create) return TABLE_SIZE;
                return firstTombstone != TABLE_SIZE ? firstTombstone : pos;
            }
            if (slot.state == 1 && sameKey(slot, key)) return pos;
            if (create && slot.state == 2 && firstTombstone == TABLE_SIZE) firstTombstone = pos;
            pos = (pos + 1) & (TABLE_SIZE - 1);
        }
        return create ? firstTombstone : TABLE_SIZE;
    }

    std::int32_t allocateNode(std::int32_t value) {
        std::int32_t index;
        if (header_.freeHead != NIL) {
            index = header_.freeHead;
            const Node freeNode = readNode(index);
            header_.freeHead = freeNode.left;
        } else {
            index = header_.nodeCount++;
        }
        writeHeader();
        writeNode(index, Node{NIL, NIL, value, 1});
        return index;
    }

    void freeNode(std::int32_t index) {
        // The left member is a next-free pointer for nodes outside a tree.
        writeNode(index, Node{header_.freeHead, NIL, 0, 0});
        header_.freeHead = index;
        writeHeader();
    }

    int height(std::int32_t index) const {
        return index == NIL ? 0 : readNode(index).height;
    }
    void updateHeight(Node &node) const {
        node.height = 1 + std::max(height(node.left), height(node.right));
    }

    std::int32_t rotateRight(std::int32_t yIndex, Node y) {
        const std::int32_t xIndex = y.left;
        Node x = readNode(xIndex);
        const std::int32_t middle = x.right;
        x.right = yIndex;
        y.left = middle;
        updateHeight(y);
        writeNode(yIndex, y);
        updateHeight(x);
        writeNode(xIndex, x);
        return xIndex;
    }

    std::int32_t rotateLeft(std::int32_t xIndex, Node x) {
        const std::int32_t yIndex = x.right;
        Node y = readNode(yIndex);
        const std::int32_t middle = y.left;
        y.left = xIndex;
        x.right = middle;
        updateHeight(x);
        writeNode(xIndex, x);
        updateHeight(y);
        writeNode(yIndex, y);
        return yIndex;
    }

    std::int32_t rebalance(std::int32_t index, Node node) {
        updateHeight(node);
        const int balance = height(node.left) - height(node.right);
        if (balance > 1) {
            const Node left = readNode(node.left);
            if (height(left.left) < height(left.right))
                node.left = rotateLeft(node.left, left);
            return rotateRight(index, node);
        }
        if (balance < -1) {
            const Node right = readNode(node.right);
            if (height(right.right) < height(right.left))
                node.right = rotateRight(node.right, right);
            return rotateLeft(index, node);
        }
        writeNode(index, node);
        return index;
    }

    std::int32_t insertNode(std::int32_t index, std::int32_t value, bool &inserted) {
        if (index == NIL) {
            inserted = true;
            return allocateNode(value);
        }
        Node node = readNode(index);
        if (value < node.value) {
            node.left = insertNode(node.left, value, inserted);
        } else if (value > node.value) {
            node.right = insertNode(node.right, value, inserted);
        } else {
            return index;
        }
        return inserted ? rebalance(index, node) : index;
    }

    std::int32_t minimum(std::int32_t index) const {
        Node node = readNode(index);
        while (node.left != NIL) {
            index = node.left;
            node = readNode(index);
        }
        return index;
    }

    std::int32_t eraseNode(std::int32_t index, std::int32_t value, bool &removed) {
        if (index == NIL) return NIL;
        Node node = readNode(index);
        if (value < node.value) {
            node.left = eraseNode(node.left, value, removed);
            return removed ? rebalance(index, node) : index;
        }
        if (value > node.value) {
            node.right = eraseNode(node.right, value, removed);
            return removed ? rebalance(index, node) : index;
        }

        removed = true;
        if (node.left == NIL || node.right == NIL) {
            const std::int32_t child = node.left != NIL ? node.left : node.right;
            freeNode(index);
            return child;
        }
        const std::int32_t successorIndex = minimum(node.right);
        const Node successor = readNode(successorIndex);
        node.value = successor.value;
        bool successorRemoved = false;
        node.right = eraseNode(node.right, successor.value, successorRemoved);
        return rebalance(index, node);
    }

    void printInOrder(std::int32_t index, bool &first) const {
        if (index == NIL) return;
        const Node node = readNode(index);
        printInOrder(node.left, first);
        if (!first) std::cout << ' ';
        std::cout << node.value;
        first = false;
        printInOrder(node.right, first);
    }
};

} // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    Database db;
    int n;
    if (!(std::cin >> n)) return 0;
    std::string command, key;
    for (int i = 0; i < n; ++i) {
        std::cin >> command >> key;
        if (command == "find") {
            db.find(key);
        } else {
            std::int32_t value;
            std::cin >> value;
            if (command == "insert") db.insert(key, value);
            else db.erase(key, value);
        }
    }
}
