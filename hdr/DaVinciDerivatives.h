#pragma once

#include <set>
#include <string>
#include <iostream>
#include <string_view>
#include <iostream>
#include <iterator>
#include <vector>
#include <regex>
#include <unordered_map>
#include <chrono>
#include <sstream>
#include <limits>
#include <stack>
#include <map>
#include <thread>
#include <future>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <iomanip>
#include <fstream>
#include <assert.h>

#include "command.h"

namespace {

// just one copy for all orders :-)
const char BuySide[] = "BUY";
const char SellSide[] = "SELL";
const char dummy[] = "dummy";

std::vector<std::string> Split(const std::string& input, const std::string& delimiter){

    std::vector<std::string> ret;
    std::stringstream ss;
    ss << "[^\\" << delimiter << "]+";
    std::regex r(ss.str());

    for (std::sregex_iterator i = std::sregex_iterator(input.begin(), input.end(), r);
         i != std::sregex_iterator(); ++i){
        std::smatch m = *i;
        ret.push_back(m.str());
    }

    return ret;
}

template <typename SIDE>
constexpr bool isValidOrderType(const SIDE& t) {
    if (t ==  BuySide || t == SellSide) {
        return true;
    }
    return false;
}

template<const char* SIDE>
class Order;

template<const char* SIDE>
using Order_ptr = std::shared_ptr<Order<SIDE>>;

template<const char* SIDE>
class Order{

public:
    Order(std::uint64_t ts, std::string s, std::uint32_t oid, uint64_t v, double p) noexcept
        : mTimestamp(ts), mSymbol(s), mOrderID(oid), mVolume(v), mPrice(p) { }

    Order(std::string s, uint64_t v = 0) noexcept :mSymbol(s), mVolume(v) { }
    Order<SIDE>& operator=(const Order& rhs) = delete;

    friend bool operator<(const Order_ptr<SIDE>& lhs, const Order_ptr<SIDE>& rhs){
        return lhs->Compare(rhs);
    }

    /*  Order Book (BUY) while SELL just ordered by Symbol and Time
     * |------------------
     * | SYMBOL   |     volume |
     * | "1"              MAX
     * | "2"              ..
     * | "3"              ..
     * | "3"              ..
     * | "3"              ..
     * | "4"              MIN
    */
    bool Compare(const Order_ptr<SIDE>& rhs) const {
        if (IsBuyOrder()){
            if (mSymbol == rhs->mSymbol){
                return mVolume > rhs->mVolume;
            }
            return mSymbol < rhs->mSymbol;
        }else{
            // assume the input is chronologically ordered
            return mSymbol < rhs->mSymbol;
        }
    }

    /*
     * quick pointer checking instead of string compare
     * bool IsBuyOrder<&BuySide>():
     *  push    rbp
     *  mov     rbp, rsp
     *  mov     eax, 1
     *  pop     rbp
     *  ret
     * main:
     *  push    rbp
     *  mov     rbp, rsp
     *  call    bool IsBuyOrder<&BuySide>()
     *  mov     eax, 0
     *  pop     rbp
     *  ret
     * [eax(return register) is set to 1 with no runtime overhead] this is critical as this function is called often
    */
    inline constexpr bool IsBuyOrder() const {
        return (mSide == BuySide);
    }

public:
    // declare private with getter/setter
    std::uint64_t mTimestamp;
    std::string mSymbol; //[PK]
    std::uint32_t mOrderID;//[UNIQUE_KEY]
    const char* mSide = SIDE;
    uint64_t mVolume; //[PK + Composite Key]
    double mPrice;
};

/*
 * multiset is guranteed to add same elements at the upper bound of equal range
 * multiset performs better than prority_queue with constant time of O(1)
*/
template<const char* SIDE, std::enable_if_t<isValidOrderType<const char*>(SIDE), bool> = false>
class OrdersContainer : public std::multiset<Order_ptr<SIDE>>{

public:
    using std::multiset<Order_ptr<SIDE>>::insert;

    friend std::ostream& operator<<(std::ostream& os, const OrdersContainer<SIDE>& oc){
        os << SIDE << ": ";
        for (const auto& i : oc){
            os << i << " ";
        }
        os << std::endl;
        return os;
    }
};

class SellOrderBookKeeping{

    struct comp {
        bool operator()(const Order_ptr<SellSide>& lhs, const Order_ptr<SellSide>& rhs) const {
            return (lhs->mPrice < rhs->mPrice);
        }
    };
    using BestPriceSellOrdersSet = std::set<Order_ptr<SellSide>, comp>;
    using BestSellPriceForSymbolHash = std::unordered_map<std::string_view/*symbol*/, BestPriceSellOrdersSet>;
    using BestSellPriceAtTimePointMap = std::map<std::uint64_t/*timestamp*/, BestSellPriceForSymbolHash>;
    using MessageType = std::tuple<Order_ptr<SellSide>, bool>;

public:
    bool Process(){

        while(true){
            std::unique_lock<std::mutex> lk(mMessageBoxMutex);
            mMessageBoxCondVar.wait(lk, [this]{ return !mMessageBox.empty(); });
            const auto [o, isInsert] = mMessageBox.front();
            if (isInsert){
                DoSellOrderInsert(o);
            }else{
                DoSellOrderCXL(o);
            }
            lk.unlock();
            mMessageBox.pop_front();
            mMessageBoxCondVar.notify_one();
        }
    }

    void AddMessage(const Order_ptr<SellSide> newOrder, const bool isInsert = true){
        std::unique_lock<std::mutex> lk(mMessageBoxMutex);
        mMessageBoxCondVar.wait(lk, [this]{ return mMessageBox.empty(); });
        mMessageBox.push_back(std::make_tuple(newOrder, isInsert));
        lk.unlock();
        mMessageBoxCondVar.notify_one();
    }

    BestSellPriceAtTimePointMap::iterator GetBestSell(std::uint64_t ts) {
        return mLookupBestSellPrice.lower_bound(ts);;
    }

private:
    inline void DoSellOrderInsert(const Order_ptr<SellSide>& newOrder){

        if (mLookupBestSellPrice.empty()){
            BestSellPriceForSymbolHash& second = mLookupBestSellPrice[newOrder->mTimestamp];
            second[newOrder->mSymbol].insert(newOrder);
        }else{
            const BestSellPriceForSymbolHash& prevTS = mLookupBestSellPrice.rbegin()->second;
            const auto [it, success] = mLookupBestSellPrice.insert({newOrder->mTimestamp, prevTS});
            BestSellPriceForSymbolHash& newTS = it->second;
            newTS[newOrder->mSymbol].insert(newOrder);
        }
    }

    inline void DoSellOrderCXL(const Order_ptr<SellSide>& newOrder){
        if (mLookupBestSellPrice.empty()){
            // Invalid: ignore
        }else{
            const BestSellPriceForSymbolHash& prevTS = mLookupBestSellPrice.rbegin()->second;
            const auto [it, success] = mLookupBestSellPrice.insert({newOrder->mTimestamp, prevTS});
            BestSellPriceForSymbolHash& newTS = it->second;
            auto& st = newTS[newOrder->mSymbol];
            for(auto it = st.begin(); it != st.end(); ) {
                if((*it)->mOrderID == newOrder->mOrderID){
                    it = st.erase(it);
                }else{
                    ++it;
                }
            }
        }
    }

    std::deque<MessageType> mMessageBox;
    std::mutex mMessageBoxMutex;
    std::condition_variable mMessageBoxCondVar;
    std::atomic_bool mExit;
    // Mayeb squize it for some regular intervals?
    BestSellPriceAtTimePointMap mLookupBestSellPrice;
};

class OrderBook{

public:
    OrderBook(){
        std::thread(&SellOrderBookKeeping::Process, std::ref(mBookKeeper)).detach();
    }
    /* CORE APIS [START] */

    /*
     * Since the OB is sorted based on (symbols + MAX volmume)  its takes log time with no additional memory
    */
    std::unordered_map<std::string_view, std::uint64_t> OrderCounts(){

        std::unordered_map<std::string_view, std::uint64_t> ret;
        if (!mBuyOrders.empty()){
            auto firstKeyItr = mBuyOrders.begin();
            for (auto currItr = firstKeyItr; currItr != mBuyOrders.end(); ) {
                // TODO Must be optimized with single call for composite key(fat key)
                Order_ptr<BuySide> tmp = std::make_shared<Order<BuySide>>((*currItr)->mSymbol, UINT64_MAX);
                const auto lb = mBuyOrders.lower_bound(tmp);
                tmp->mVolume = 0;
                const auto ub = mBuyOrders.upper_bound(tmp);
                ret[(*lb)->mSymbol] = std::distance(lb, ub);
                currItr = ub;
            }
        }

        if (!mSellOrders.empty()){
            auto firstKeyItr = mSellOrders.begin();
            for (auto currItr = firstKeyItr; currItr != mSellOrders.end(); ) {
                Order_ptr<SellSide> tmp = std::make_shared<Order<SellSide>>((*currItr)->mSymbol);
                const auto [lb, ub] = mSellOrders.equal_range(tmp);
                ret[(*lb)->mSymbol] += std::distance(lb, ub);
                currItr = ub;
            }
        }

        return ret;
    }

    /*
     * Since the BUY orders in OB are sorted in descending order of hash(symbols, volumes) its takes (log + constant) time to take from top
    */
    std::vector<Order_ptr<BuySide>> BiggestBuyOrders(const std::string& symbol, const int top){

        std::vector<Order_ptr<BuySide>> ret;
        ret.reserve(top);
        Order_ptr<BuySide> tmp = std::make_shared<Order<BuySide>>(symbol, UINT64_MAX);
        const auto lb = mBuyOrders.lower_bound(tmp);
        tmp->mVolume = 0;
        const auto ub = mBuyOrders.upper_bound(tmp);
        OrdersContainer<BuySide>::const_iterator currItr = lb;
        for (int currCount = 0; (currItr != ub && currCount++ < top) ; ++currItr){
            ret.emplace_back(*currItr);
        }

        return ret;
    }

    /*
     * Sell price must be lowest for maximum profit. lets have whole Order returned just in case need more than volume+price info in future(Open Closed Principle)
     * 2 approaches:
     * #1 - store the lowest price of every symbol for every transaction(time point).
     *      pros:                   cons:
     *      quick turnaround O(1)   extra memory
     *      ----------------------------------------------
     *      [timepoint]   [symbol] [orderID of best sell]
     *      ---------------------------------------------
     *      9'0 clock        "1"             1[$5]
     *                       "2"             2[$5]
     *                       "3"             3
     *      10'0 clock       "1"             11[$4]->1[$5]
     *                       "2"             22[$4]->2[$5]
     *                       "3"             3
     *                       "4"             44
     *      11'0 clock       "1"             11[$4]->1[$5]->5[$6]   // ID: 5 added
     *                       "2"             22[$4]->2[$5]
     *                       "3"             3
     *                       "4"             44
     *      12'0 clock       "1"             11[$4]->5[$6]         // ID: 1 cancelled
     *                       "2"             2[$5]                 // ID: 22 cancelled
     *                       "3"             3
     *                       "4"             44
     * #2 - rollback the order book to the said time point. using either Command or Memento design patterns
     *      Command pattern: every doAmend() calls will be stacked up for undoAmend()
     *      Memento pattern: cache the OrderBook (of shared pointers) at various time point(or transactions)
     *      pros:               cons:
     *      lazy turnaround     no (minimum) extra memory
     *
     *  Implementing #1 for now at constant time complexity O(1)
    */
    Order_ptr<SellSide> BestSellAtTime(const std::string& symbol, const std::string& timestamp){

        Order_ptr<SellSide> minOrder;
        // Below code is to query at the end of trade day
#if 0
        double minPrice = std::numeric_limits<double>::max();
        Order_ptr<SellSide> tmp = std::make_shared<Order<SellSide>>(symbol);
        const auto [lb, ub] = mSellOrders.equal_range(tmp);
        std::uint64_t ts = getTime(timestamp) - mMidNightInMS;
        // std::find_if performs better than std::min_element
        std::find_if(lb, ub, [ts, &minPrice, &minOrder](const Order_ptr<SellSide>& item){

            if (item->mTimestamp > ts)
                return true;

            // std::min(minPrice, item->mPrice)
            if (item->mPrice < minPrice){
                minPrice = item->mPrice;
                minOrder = item;
            }
            return false;
        });
#else
        // Below code is to query within the trade day
        std::uint64_t ts = getTime(timestamp) - mMidNightInMS;
        auto itr = mBookKeeper.GetBestSell(ts);
        const auto& st = itr->second[symbol];
        if (const auto& st = itr->second[symbol]; !st.empty()){
            minOrder = *(st.begin());
        }
#endif
        return minOrder;
    }

    /* CORE APIS [END] */

    // BUY
    inline void DoBuyOrderInsert(const Order_ptr<BuySide>& newOrder){
        auto itr = mBuyOrders.insert(newOrder);
        mLookupBuyOrders[newOrder->mOrderID] = itr;
    }

    inline void DoBuyOrderCXL(const Order_ptr<BuySide>& newOrder){
        auto Buyitr = mLookupBuyOrders.find(newOrder->mOrderID);
        if (Buyitr != mLookupBuyOrders.end()){
            mBuyOrders.erase(Buyitr->second);
        }
    }

    inline void DoBuyOrderCRP(const Order_ptr<BuySide>& newOrder){
        DoBuyOrderCXL(newOrder);
        DoBuyOrderInsert(newOrder);
    }

    // SELL
    inline void DoSellOrderInsert(const Order_ptr<SellSide>& newOrder){
        mBookKeeper.AddMessage(newOrder);
        auto itr = mSellOrders.insert(newOrder);
        mLookupSellOrders[newOrder->mOrderID] = itr;
    }

    inline void DoSellOrderCXL(const Order_ptr<SellSide>& newOrder){
        mBookKeeper.AddMessage(newOrder, false);
        auto Sellitr = mLookupSellOrders.find(newOrder->mOrderID);
        if (Sellitr != mLookupSellOrders.end()){
            mSellOrders.erase(Sellitr->second);
        }
    }

    inline void DoSellOrderCRP(const Order_ptr<SellSide>& newOrder){
        // Do not modify the pointer. choronologial order will be lost so operator= is deleted
        DoSellOrderCXL(newOrder);
        DoSellOrderInsert(newOrder);
    }

    template<const char* SIDE>
    Order_ptr<SIDE> GetNewOrder(const std::vector<std::string>& v) noexcept{

        std::uint64_t time_since_start_of_day_ms = getTime(v[0]) - mMidNightInMS;
        return std::make_shared<Order<SIDE>>(time_since_start_of_day_ms, v[1], (std::uint64_t)std::atoi(v[2].data()),
                                              (std::uint64_t)std::atoi(v[5].data()), (double)std::stod(v[6].data()));
    }

private:
    /*
     * Processor is faster at numeric compare than string compare
    */
    std::uint64_t getTime(const std::string& t) noexcept{

        // Eg: 14:17:21.877391
        const auto& v = Split(t, ".");
        std::istringstream ss(v[0]);
        time_t tmp{0};
        struct tm tmm = *localtime(&tmp);
        ss >> std::get_time(&tmm, "%H:%M:%S");
        if (ss.fail()) {
            return 0;
        }else {
            std::time_t time = std::mktime(&tmm);
            std::chrono::system_clock::time_point t = std::chrono::system_clock::from_time_t(time);
            auto t_ms = std::chrono::time_point_cast<std::chrono::microseconds>(t);
            std::uint64_t t_f = t_ms.time_since_epoch().count();
            if (v.size() >= 2){
                try{
                    std::size_t tt_ms = std::atoi(v[1].data());
                    return (t_f + tt_ms);
                }catch(...){
                    return 0;
                }
            }
            return t_f;
        }
    }

    // reduced timestamp size by calculating from midnight instead of time_since_epoch in microseconds for accuracy
    const std::uint64_t mMidNightInMS = getTime("00:00:00");
    // BST for storage and hash map for fast look up.
    OrdersContainer<BuySide>  mBuyOrders;
    OrdersContainer<SellSide>  mSellOrders;
    std::unordered_map<std::uint32_t/*orderID*/, OrdersContainer<BuySide>::iterator> mLookupBuyOrders;
    std::unordered_map<std::uint32_t/*orderID*/, OrdersContainer<SellSide>::iterator> mLookupSellOrders;
    SellOrderBookKeeping mBookKeeper;
};

// As per SOLID Design Principles. Strong exception safety
class DaVinciOrderMatchingEngine : public Command
{

public:
    void execute(){

        // Eg: 14:17:21.877391;DVAM1;00000001;I;BUY;100;12.5
        static const std::string file{"../DaVinci_test_data.txt"};  // SSO
        std::ifstream input;
        try{
            input.open(file);
            for (std::string line; std::getline(input, line, '\n'); ) {
                const auto& v = Split(line, ";");
                assert(v.size() == 7);
                DoOrder(v);
            }
        }catch (...) {
            input.close();
        }

        input.close();

        std::cout << "API 1" << std::endl;
        std::cout << "OrderCounts: " << std::endl;
        for (const auto& i : mOB.OrderCounts()){
            std::cout << "Symbol: " << i.first << " Count: " << i.second << std::endl;
        }

        std::cout << "API 2" << std::endl;
        std::cout << "top 3 BiggestBuyOrders for DVAM1: " << std::endl;
        for (const Order_ptr<BuySide>& i : mOB.BiggestBuyOrders("DVAM1", 3)){
            std::cout << "Order ID: " << i->mOrderID << " Volume: " << i->mVolume << std::endl;
        }

        std::cout << "API 3" << std::endl;
        std::cout << "BestSellAtTime for TEST0 unitl 15:30:00: " << std::endl;
        const Order_ptr<SellSide>& s = mOB.BestSellAtTime("TEST0", "15:30:00");
        if (!s){
            std::cout << "TEST0 has been cancelled or never placed until 15:30:00" << std::endl;
        }else{
            std::cout << "Sell Price: " << s->mPrice << " Volume: " << s->mVolume << std::endl;
        }
    }

private:
    void DoOrder(const std::vector<std::string>& v){

        // non-type template parameter to seperate out buy/sell side at compile time.
        if (IsBuy(v[4])){

            Order_ptr<BuySide> o = mOB.GetNewOrder<BuySide>(v);
            if (IsInsert(v[3])){
                mOB.DoBuyOrderInsert(o);
            }else if (IsCancel(v[3])){
                mOB.DoBuyOrderCXL(o);
            }else if (IsAmend(v[3])){
                mOB.DoBuyOrderCRP(o);
            }
        }else{

            Order_ptr<SellSide> o = mOB.GetNewOrder<SellSide>(v);
            if (IsInsert(v[3])){
                mOB.DoSellOrderInsert(o);
            }else if (IsCancel(v[3])){
                mOB.DoSellOrderCXL(o);
            }else if (IsAmend(v[3])){
                mOB.DoSellOrderCRP(o);
            }
        }
    }

    inline bool IsBuy(const std::string& ID){
        return (ID == "BUY");
    }

    inline bool IsInsert(const std::string& OP){
        return (OP == "I");
    }

    inline bool IsCancel(const std::string& OP){
        return (OP == "C");
    }

    inline bool IsAmend(const std::string& OP){
        return (OP == "A");
    }

    OrderBook mOB;
};

}// anonymous namespace will shrink binary size by NOT exporting inline functions to as they are static to this file
