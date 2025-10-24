#include <databento/dbn_file_store.hpp>
#include <databento/enums.hpp>
#include <databento/record.hpp>
#include <iostream>
#include <iomanip>

using namespace databento;

int main() {
    const std::string path = "../../src/data/CLX5_mbo.dbn";  // adjust to your path
    std::cout << "📂 Reading DBN file: " << path << std::endl;

    try {
        DbnFileStore store(path);
        auto metadata = store.GetMetadata();

        std::cout << "✅ Schema: " << ToString(metadata.schema.value()) << std::endl;
        std::cout << "✅ Symbols: ";
        for (const auto& s : metadata.symbols) std::cout << s << " ";
        std::cout << "\n✅ Dataset: " << metadata.dataset << std::endl;
        std::cout << "\n--- First 10 MBO Records ---\n";

        int count = 0;
        while (const auto* record = store.NextRecord()) {
            if (record->RType() == RType::Mbo) {
                const auto& mbo = record->Get<MboMsg>();
                std::cout << std::fixed << std::setprecision(6)
                          << "[" << count + 1 << "] "
                          << "ts_event=" << mbo.hd.ts_event.time_since_epoch().count() << " | "
                          << "side=" << static_cast<char>(mbo.side) << " | "
                          << "price=" << mbo.price << " | "
                          << "size=" << mbo.size << " | "
                          << "action=" << static_cast<char>(mbo.action) << " | "
                          << "order_id=" << mbo.order_id << std::endl;
                count++;
            }
            if (count >= 10) break;
        }

        if (count == 0)
            std::cout << "⚠️  No MBO records found.\n";
        else
            std::cout << "✅ Done.\n";

    } catch (const std::exception& e) {
        std::cerr << "❌ Error reading DBN file: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}