#include <client/params.hpp>
#include <csnode/configholder.hpp>

#include <base58.h>

#include <cctype>
#include "apihandler.hpp"
#include "tokens.hpp"
#include "smartcontracts.hpp"

#ifdef TOKENS_CACHE

// An amount argument: v0 tokens take it as a string, v1 and v2 tokens as a BigDecimal
static inline bool isAmountParam(const general::Variant& v) {
    return v.__isset.v_string || v.__isset.v_big_decimal;
}

static inline const std::string& amountParam(const general::Variant& v) {
    return v.__isset.v_big_decimal ? v.v_big_decimal : v.v_string;
}

static inline bool isNormalTransfer(const std::string& method, const std::vector<general::Variant>& params) {
    return method == "transfer" && params.size() == 2 && params[0].__isset.v_string && isAmountParam(params[1]);
}

static inline bool isTransferFrom(const std::string& method, const std::vector<general::Variant>& params) {
    return method == "transferFrom" && params.size() == 3 && params[0].__isset.v_string && params[1].__isset.v_string &&
           isAmountParam(params[2]);
}

static csdb::Address tryExtractPublicKey(const std::string& str) {
    // First try to decipher
    csdb::Address result;
    std::vector<uint8_t> vc;
    if (str.size() > 64) {
        return result;
    }
    bool decodeSucc = DecodeBase58(str, vc);
    if (!decodeSucc || vc.size() != cscrypto::kPublicKeySize)
        return result;

    return csdb::Address::from_public_key(vc);
}

static csdb::Address tryGetRegisterData(const std::string& method, const std::vector<general::Variant>& params) {
    if (method == "register" && params.size() == 1 && params[0].__isset.v_string)
        return tryExtractPublicKey(params[0].v_string);

    return csdb::Address();
}

// A plain decimal ("12.5") of a decimal string or of a Java BigDecimal.toString() ("1.5E+3"),
// without leading or trailing zeros; "0" for anything else, a negative amount included
static std::string tryExtractAmount(const std::string& str) {
    if (str.empty() || str.size() > 256)
        return "0";

    std::string digits;
    size_t pointPos = std::string::npos;  // number of digits before the separator
    size_t i = 0;
    if (str[0] == '+')
        ++i;
    for (; i < str.size(); ++i) {
        const char c = str[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        }
        else if ((c == '.' || c == ',') && pointPos == std::string::npos) {
            pointPos = digits.size();
        }
        else {
            break;
        }
    }
    if (digits.empty())
        return "0";

    long exponent = 0;
    if (i < str.size()) {
        if (str[i] != 'e' && str[i] != 'E')
            return "0";
        ++i;
        bool negative = false;
        if (i < str.size() && (str[i] == '+' || str[i] == '-')) {
            negative = str[i] == '-';
            ++i;
        }
        if (i == str.size() || str.size() - i > 4)
            return "0";
        for (; i < str.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(str[i])))
                return "0";
            exponent = exponent * 10 + (str[i] - '0');
        }
        if (negative)
            exponent = -exponent;
    }

    long intLen = static_cast<long>(pointPos == std::string::npos ? digits.size() : pointPos) + exponent;
    if (intLen > 512 || intLen < -512)
        return "0";

    std::string integral, fraction;
    if (intLen <= 0) {
        fraction = std::string(static_cast<size_t>(-intLen), '0') + digits;
    }
    else if (static_cast<size_t>(intLen) >= digits.size()) {
        integral = digits + std::string(static_cast<size_t>(intLen) - digits.size(), '0');
    }
    else {
        integral = digits.substr(0, static_cast<size_t>(intLen));
        fraction = digits.substr(static_cast<size_t>(intLen));
    }

    const auto firstNonZero = integral.find_first_not_of('0');
    integral = firstNonZero == std::string::npos ? "0" : integral.substr(firstNonZero);
    const auto lastNonZero = fraction.find_last_not_of('0');
    fraction = lastNonZero == std::string::npos ? "" : fraction.substr(0, lastNonZero + 1);

    return fraction.empty() ? integral : integral + "." + fraction;
}

template <typename T>
T getVariantAs(const general::Variant&);
template <>
std::string getVariantAs(const general::Variant& var) {
    if (var.__isset.v_big_decimal)
        return var.v_big_decimal;
    if (var.__isset.v_string)
        return var.v_string;
    if (var.__isset.v_int)
        return std::to_string(var.v_int);
    if (var.__isset.v_int_box)
        return std::to_string(var.v_int_box);
    if (var.__isset.v_long)
        return std::to_string(var.v_long);
    if (var.__isset.v_long_box)
        return std::to_string(var.v_long_box);
    return std::string{};
}

template <typename RetType>
void executeAndCall(api::APIHandler* p_api, const general::Address& addr, const general::Address& addr_smart, const std::vector<general::ByteCodeObject>& byteCodeObjects,
                    const std::string& state, const std::string& method, const std::vector<general::Variant>& params, const std::function<void(const RetType&)> handler) {
    executor::ExecuteByteCodeResult result;

    if (byteCodeObjects.empty())
        return;
    std::vector<executor::MethodHeader> methodHeader;
    {
        executor::MethodHeader tmp;
        tmp.methodName = method;
        tmp.params = params;
        methodHeader.push_back(tmp);
    }
    p_api->getExecutor().executeByteCode(result, addr, addr_smart, byteCodeObjects, state, methodHeader, true /*isGetter*/, cs::Executor::kUseLastSequence);

    if (!result.status.code && !result.results.empty() && !result.results[0].status.code)
        handler(getVariantAs<RetType>(result.results[0].ret_val));
}

// balanceOf asked for this many holders at once: the node waits executor_receive_timeout for the
// whole answer, and the executor deserializes the state once per holder
static constexpr size_t kBalancesPerRequest = 50;

/* Call under data lock only */
void TokensMaster::refreshTokenState(const csdb::Address& token, const std::string& newState, bool checkBalance, const std::set<csdb::Address>& only) {
    bool present = false;
    auto byteCodeObjects = api_->getSmartByteCode(token, present);
    if (!present || byteCodeObjects.empty() || newState.empty()) return;

    auto& t = tokens_[token];
    const csdb::Address deployer = t.owner;

    general::Address addr   = std::string((char*)token.public_key().data(), token.public_key().size());
    general::Address dpAddr = std::string((char*)deployer.public_key().data(), deployer.public_key().size());

    // a getter that fails keeps the value known before
    executeAndCall<std::string>(api_, dpAddr, addr, byteCodeObjects, newState, "getName", std::vector<general::Variant>(),
                                [&t](const std::string& newName) { t.name = newName.substr(0, 255); });

    executeAndCall<std::string>(api_, dpAddr, addr, byteCodeObjects, newState, "totalSupply", std::vector<general::Variant>(),
        [&t](const std::string& newSupp) { t.totalSupply = tryExtractAmount(newSupp); });

    executeAndCall<std::string>(api_, dpAddr, addr, byteCodeObjects, newState, "getSymbol", std::vector<general::Variant>(),
        [&t](const std::string& newSymb) {
            t.symbol.clear();
            for (uint32_t i = 0; i < newSymb.size(); ++i) {
                if (i >= 4) break;
                t.symbol.push_back((char)std::toupper(newSymb[i]));
            }
        });

    if (!checkBalance) {
        return;
    }

    std::vector<csdb::Address> holders;
    if (only.empty()) {
        holders.reserve(t.holders.size());
        for (auto& h : t.holders)
            holders.push_back(h.first);
    }
    else {
        for (auto& h : only) {
            if (t.holders.count(h))
                holders.push_back(h);
        }
    }

    executor::SmartContractBinary smartContractBinary;
    smartContractBinary.contractAddress = addr;
    smartContractBinary.object.byteCodeObjects = byteCodeObjects;
    smartContractBinary.object.instance = newState;
    smartContractBinary.stateCanModify = 0;

    for (size_t from = 0; from < holders.size(); from += kBalancesPerRequest) {
        const size_t to = std::min(holders.size(), from + kBalancesPerRequest);
        std::vector<std::vector<general::Variant>> holderKeysParams;
        holderKeysParams.reserve(to - from);
        for (size_t i = from; i < to; ++i) {
            general::Variant var;
            auto key = holders[i].public_key();
            var.__set_v_string(EncodeBase58(cs::Bytes(key.begin(), key.end())));
            holderKeysParams.push_back(std::vector<general::Variant>(1, var));
        }

        executor::ExecuteByteCodeMultipleResult result;
        api_->getExecutor().executeByteCodeMultiple(result, dpAddr, smartContractBinary, "balanceOf", holderKeysParams, 100, cs::Executor::kUseLastSequence);
        if (result.status.code || result.results.size() != to - from) {
            continue;
        }
        for (size_t i = from; i < to; ++i) {
            const auto& res = result.results[i - from];
            if (!res.status.code) {
                t.holders[holders[i]].balance = tryExtractAmount(getVariantAs<std::string>(res.ret_val));
            }
        }
    }

    t.realHoldersCount = 0;
    for (const auto& h : t.holders) {
        if (!isZeroAmount(h.second.balance))
            ++t.realHoldersCount;
    }
}

/* Call under data lock only */
void TokensMaster::initiateHolder(Token& token, const csdb::Address& address, const csdb::Address& holder, bool increaseTransfers /* = false*/) {
    if (!holder.is_valid()) {
        return;
    }
    if (increaseTransfers)
        ++token.holders[holder].transfersCount;
    else
        token.holders[holder];

    holders_[holder].insert(address);
}

TokensMaster::TokensMaster(api::APIHandler* api)
: api_(api) {
#if defined(MONITOR_NODE) || defined(WEB_WALLET_NODE)
    enabled_ = true;
#else
    enabled_ = cs::ConfigHolder::instance().config()->getApiSettings().tokensCache;
#endif
    if (enabled_) {
        cslog() << "API: token cache on";
    }
}

TokensMaster::~TokensMaster() {
}

void TokensMaster::updateTokenChaches(const csdb::Address& addr, const std::string& newState, const TokenInvocationData::Params& ps, bool refresh) {
    if (!enabled_)
        return;
    std::lock_guard<decltype(dataMut_)> lInt(dataMut_);
    auto tIt = tokens_.find(addr);
    if (tIt == tokens_.end())
        return;  // Ignore if not-a-token

    // the wallets whose balance the call may have changed: the initiator and every address among
    // the arguments (the recipient of a transfer, the spender of an approval, a registered
    // wallet, or the arguments of a method outside the standard such as mint or airdrop)
    std::set<csdb::Address> touched;
    touched.insert(ps.initiator);
    initiateHolder(tIt->second, tIt->first, ps.initiator);
    ++tIt->second.transactionsCount;

    if (!ps.method.empty()) {
        if (isTransfer(ps.method, ps.params)) {
            ++tIt->second.transfersCount;
            auto trPair = getTransferData(ps.initiator, ps.method, ps.params);
            initiateHolder(tIt->second, tIt->first, trPair.first, true);
            initiateHolder(tIt->second, tIt->first, trPair.second, true);
        }
        else if (isExtended(tIt->second.tokenStandard)) {
            csdb::Address regDude = tryGetRegisterData(ps.method, ps.params);
            initiateHolder(tIt->second, tIt->first, regDude);
        }
        for (const auto& p : ps.params) {
            if (p.__isset.v_string) {
                const csdb::Address a = tryExtractPublicKey(p.v_string);
                if (a.is_valid()) {
                    initiateHolder(tIt->second, tIt->first, a);
                    touched.insert(a);
                }
            }
        }
    }

    if (refresh && api_->isBDLoaded()) {
        const std::string state = newState.empty() ? cs::SmartContracts::get_contract_state(api_->get_s_blockchain(), addr) : newState;
        // the token's own rules decide the balances: ask the contract for those of the wallets the
        // call involves; a method outside the standard may move other balances too, so a token
        // with few holders is read in full
        refreshTokenState(addr, state, true, tIt->second.holders.size() <= kBalancesPerRequest ? std::set<csdb::Address>{} : touched);
    }
}

void TokensMaster::checkNewDeploy(const csdb::Address& sc, const csdb::Address& deployer, const api::SmartContractInvocation& sci) {
    if (!enabled_)
        return;
    DeployTask dt;
    dt.address = sc;
    dt.deployer = deployer;
    dt.byteCodeObjects = sci.smartContractDeploy.byteCodeObjects;

    executor::GetContractMethodsResult methodsResult;
    if (!dt.byteCodeObjects.empty()) {
        api_->getExecutor().getContractMethods(methodsResult, dt.byteCodeObjects);
        if (!methodsResult.status.code) {
            if (methodsResult.tokenStandard != TokenStandard::NotAToken) {
                std::lock_guard<decltype(dataMut_)> lInt(dataMut_);
                // a deploy is seen once; never reset a token known already
                if (tokens_.count(dt.address) == 0) {
                    Token t;
                    t.tokenStandard = methodsResult.tokenStandard;
                    t.owner = dt.deployer;
                    tokens_[dt.address] = t;
                }
            }
        }
    }
}

void TokensMaster::checkNewState(const csdb::Address& sc, const csdb::Address& initiator, const api::SmartContractInvocation& sci, const std::string& newState,
                                 bool refresh) {
    TokenInvocationData::Params ps;
    ps.initiator = initiator;
    ps.method = sci.method;
    ps.params = sci.params;
    updateTokenChaches(sc, newState, ps, refresh);
}

void TokensMaster::loadTokenInfo(const std::function<void(const TokensMap&, const HoldersMap&)> func) {
    std::lock_guard<decltype(dataMut_)> l(dataMut_);
    func(tokens_, holders_);
}

void TokensMaster::refreshAll() {
    if (!enabled_)
        return;
    std::vector<csdb::Address> tokens;
    {
        std::lock_guard<decltype(dataMut_)> l(dataMut_);
        tokens.reserve(tokens_.size());
        for (const auto& t : tokens_)
            tokens.push_back(t.first);
    }
    cslog() << "tokens are loading(" << tokens.size() << ")...";
    size_t done = 0;
    for (const auto& token : tokens) {
        const std::string state = cs::SmartContracts::get_contract_state(api_->get_s_blockchain(), token);
        {
            std::lock_guard<decltype(dataMut_)> l(dataMut_);
            refreshTokenState(token, state, true);
        }
        if (tokens.size() > 100 && ++done % (tokens.size() / 10) == 0)
            cslog() << "loading tokens: " << 100 * done / tokens.size() << "%";
    }
    cslog() << "tokens loaded!";
}

bool TokensMaster::isTransfer(const std::string& method, const std::vector<general::Variant>& params) {
    return isNormalTransfer(method, params) || isTransferFrom(method, params);
}

using AddrPair = std::pair<csdb::Address, csdb::Address>;

AddrPair TokensMaster::getTransferData(const csdb::Address& initiator, const std::string& method, const std::vector<general::Variant>& params) {
    AddrPair result;

    if (isNormalTransfer(method, params)) {
        result.first = initiator;
        result.second = tryExtractPublicKey(params[0].v_string);
    }
    else if (isTransferFrom(method, params)) {
        result.first = tryExtractPublicKey(params[0].v_string);
        result.second = tryExtractPublicKey(params[1].v_string);
    }

    return result;
}

std::string TokensMaster::getAmount(const api::SmartContractInvocation& sci) {
    if (isNormalTransfer(sci.method, sci.params))
        return tryExtractAmount(amountParam(sci.params[1]));
    else if (isTransferFrom(sci.method, sci.params))
        return tryExtractAmount(amountParam(sci.params[2]));

    return "0";
}

#else

TokensMaster::TokensMaster(api::APIHandler*) {
}
TokensMaster::~TokensMaster() {
}
void TokensMaster::checkNewDeploy(const csdb::Address&, const csdb::Address&, const api::SmartContractInvocation&) {
}
void TokensMaster::checkNewState(const csdb::Address&, const csdb::Address&, const api::SmartContractInvocation&, const std::string&, bool) {
}
void TokensMaster::loadTokenInfo(const std::function<void(const TokensMap&, const HoldersMap&)>) {
}
void TokensMaster::refreshAll() {
}
bool TokensMaster::isTransfer(const std::string&, const std::vector<general::Variant>&) {
    return false;
}
std::pair<csdb::Address, csdb::Address> TokensMaster::getTransferData(const csdb::Address&, const std::string&, const std::vector<general::Variant>&) {
    return std::pair<csdb::Address, csdb::Address>();
}
std::string TokensMaster::getAmount(const api::SmartContractInvocation&) {
    return "";
}
#endif
