#include <fstream>
#include <sstream>
#include <exception>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <csnode/archive_stream.hpp>

#include <csnode/roundstat_serializer.hpp>
#include <csnode/serializers_helper.hpp>
#include <csnode/roundstat.hpp>
#include "logger.hpp"

namespace {
    const std::string kDataFileName = "roundstat.dat";
    const std::string kLogPrefix = "RoundStat_Serializer: ";
} // namespace

namespace cs {
    void RoundStat_Serializer::bind(RoundStat& roundStat) {
        roundStat_ = &roundStat;
        csdebug() << "Roundstat bindings made";
    }

    void RoundStat_Serializer::clear(const std::filesystem::path& rootDir) {
        (void)rootDir;
        roundStat_->clear();
    }


    void RoundStat_Serializer::printClassInfo() {
        roundStat_->printClassInfo();


    }

    void RoundStat_Serializer::save(const std::filesystem::path& rootDir) {
        std::ofstream ofs(rootDir / kDataFileName, std::ios::binary);
        cs::ArchiveOutput out(ofs);   // the same header bytes on every system
        boost::archive::binary_oarchive oa(out);
        csdebug() << kLogPrefix << __func__;
        roundStat_->printClassInfo();
        oa << roundStat_->serialize();
    }

    ::cscrypto::Hash RoundStat_Serializer::hash() {
        {
            std::ofstream ofs(kDataFileName, std::ios::binary);
            {
                boost::archive::binary_oarchive oa(
                    ofs,
                    boost::archive::no_header | boost::archive::no_codecvt
                );
                oa << roundStat_->serialize();
                //printClassInfo();
            }
        }

        auto result = SerializersHelper::getHashFromFile(kDataFileName);
        //std::filesystem::remove(kDataFileName);
        return result;
    }


    void RoundStat_Serializer::load(const std::filesystem::path& rootDir) {
        std::ifstream ifs(rootDir / kDataFileName, std::ios::binary);
        cs::ArchiveInput in(ifs);   // also a checkpoint written on another system
        boost::archive::binary_iarchive ia(in);
        csdebug() << kLogPrefix << __func__;
        Bytes data;
        ia >> data;
        roundStat_->deserialize(data);
        printClassInfo();
    }
}