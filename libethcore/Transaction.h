/*
    This file is part of cpp-ethereum.

    cpp-ethereum is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    cpp-ethereum is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "TransactionReceipt.h"
#include <libdevcore/FixedHash.h>
#include <libdevcore/Guards.h>
#include <libdevcore/RLP.h>
#include <libdevcrypto/Common.h>
#include <libdevcrypto/CryptoInterface.h>
#include <libethcore/Common.h>
#include <tbb/concurrent_unordered_set.h>
#include <boost/optional.hpp>


namespace dev
{
namespace eth
{
struct EVMSchedule;

/// Named-boolean type to encode whether a signature be included in the
/// serialisation process.
enum IncludeSignature
{
    WithoutSignature = 0,  ///< Do not include a signature.
    WithSignature = 1,     ///< Do include a signature.
};

enum class CheckTransaction
{
    None,
    Cheap,
    Everything
};

const int c_fieldCountRC1WithOutSig = 7;
const int c_fieldCountRC2WithOutSig = 10;
const int c_sigCount = 3;

/// function called after the transaction has been submitted
class Block;

struct NodeTransactionMarker
{
public:
    NodeTransactionMarker() = default;
    void appendNodeContainsTransaction(dev::h512 const& _node)
    {
        WriteGuard l(x_nodeListWithTheTransaction);
        m_nodeListWithTheTransaction.insert(_node);
    }

    template <typename T>
    void appendNodeListContainTransaction(T const& _nodeList)
    {
        WriteGuard l(x_nodeListWithTheTransaction);
        for (auto const& node : _nodeList)
        {
            m_nodeListWithTheTransaction.insert(node);
        }
    }

    bool isTheNodeContainsTransaction(dev::h512 const& _node)
    {
        ReadGuard l(x_nodeListWithTheTransaction);
        return m_nodeListWithTheTransaction.count(_node);
    }

    bool isKnownBySomeone()
    {
        ReadGuard l(x_nodeListWithTheTransaction);
        return !m_nodeListWithTheTransaction.empty();
    }

    void clear()
    {
        WriteGuard l(x_nodeListWithTheTransaction);
        m_nodeListWithTheTransaction.clear();
    }

private:
    mutable dev::SharedMutex x_nodeListWithTheTransaction;
    // Record the node where the transaction exists
    std::set<dev::h512> m_nodeListWithTheTransaction;
};

using RPCCallback = std::function<void(LocalisedTransactionReceipt::Ptr, dev::bytesConstRef input,
    std::shared_ptr<dev::eth::Block> _blockPtr)>;
/// Encodes a transaction, ready to be exported to or freshly imported from RLP.
class Transaction
{
public:
    using Ptr = std::shared_ptr<Transaction>;
    /// There are only two possible values for the v value generated by the
    /// transaction signature, 27 or 28, but the v value in vrs only two
    /// possibilities, 0 and 1. VBase - 27 Means an operation that changes to 0
    /// or 1.

    /// Constructs a null transaction.
    Transaction() {}
    /// Constructs an unsigned message-call transaction.
    Transaction(u256 const& _value, u256 const& _gasPrice, u256 const& _gas, Address const& _dest,
        bytes const& _data, u256 const& _nonce = u256(0), u256 _chainId = u256(1),
        u256 _groupId = u256(1))
      : m_type(MessageCall),
        m_nonce(_nonce),
        m_value(_value),
        m_receiveAddress(_dest),
        m_gasPrice(_gasPrice),
        m_gas(_gas),
        m_data(_data),
        m_rpcCallback(nullptr),
        m_rlpBuffer(bytes()),
        m_chainId(_chainId),
        m_groupId(_groupId)
    {}

    /// Constructs an unsigned contract-creation transaction.
    Transaction(u256 const& _value, u256 const& _gasPrice, u256 const& _gas, bytes const& _data,
        u256 const& _nonce = u256(0), u256 _chainId = u256(1), u256 _groupId = u256(1))
      : m_type(ContractCreation),
        m_nonce(_nonce),
        m_value(_value),
        m_gasPrice(_gasPrice),
        m_gas(_gas),
        m_data(_data),
        m_rpcCallback(nullptr),
        m_rlpBuffer(bytes()),
        m_chainId(_chainId),
        m_groupId(_groupId)
    {}

    /// Constructs a transaction from the given RLP.
    explicit Transaction(bytesConstRef _rlp, CheckTransaction _checkSig);

    /// Constructs a transaction from the given RLP.
    explicit Transaction(bytes const& _rlp, CheckTransaction _checkSig)
      : Transaction(&_rlp, _checkSig)
    {}
    Transaction(Transaction const&) = delete;

    Transaction& operator=(Transaction const&) = delete;

    /// Checks equality of transactions.
    bool operator==(Transaction const& _c) const
    {
        return m_type == _c.m_type &&
               (m_type == ContractCreation || m_receiveAddress == _c.m_receiveAddress) &&
               m_value == _c.m_value && m_data == _c.m_data;
    }
    /// Checks inequality of transactions.
    bool operator!=(Transaction const& _c) const { return !operator==(_c); }

    /// @returns sender of the transaction from the signature (and hash).
    /// @throws TransactionIsUnsigned if signature was not initialized
    Address const& sender() const;
    /// Like sender() but will never throw. @returns a null Address if the
    /// signature is invalid.
    Address const& safeSender() const noexcept;
    /// Force the sender to a particular value. This will result in an invalid
    /// transaction RLP.
    void forceSender(Address const& _a) { m_sender = _a; }

    /// @returns true if transaction is non-null.
    explicit operator bool() const { return m_type != NullTransaction; }

    /// @returns true if transaction is contract-creation.
    bool isCreation() const { return m_type == ContractCreation; }

    /// Serialises this transaction to an RLPStream.
    /// @throws TransactionIsUnsigned if including signature was requested but it
    /// was not initialized void streamRLP(RLPStream& _s, IncludeSignature _sig =
    /// WithSignature) const;
    void encode(bytes& _trans, IncludeSignature _sig = WithSignature) const;
    void decode(bytesConstRef tx_bytes, CheckTransaction _checkSig = CheckTransaction::Everything);
    void decode(RLP const& rlp, CheckTransaction _checkSig = CheckTransaction::Everything);
    /// @returns the RLP serialisation of this transaction.
    bytes rlp(IncludeSignature _sig = WithSignature) const
    {
        if (m_rlpBuffer != bytes())
        {
            return m_rlpBuffer;
        }
        bytes out;
        encode(out, _sig);
        return out;
    }

    /// @returns the hash of the RLP serialisation of this transaction.
    h256 hash(IncludeSignature _sig = WithSignature) const;

    /// @returns the amount of ETH to be transferred by this (message-call)
    /// transaction, in Wei. Synonym for endowment().
    u256 value() const { return m_value; }

    /// @returns the base fee and thus the implied exchange rate of ETH to GAS.
    u256 gasPrice() const { return m_gasPrice; }

    /// @returns the total gas to convert, paid for from sender's account. Any
    /// unused gas gets refunded once the contract is ended.
    u256 gas() const { return m_gas; }

    /// @returns the receiving address of the message-call transaction (undefined
    /// for contract-creation transactions).
    Address receiveAddress() const { return m_receiveAddress; }

    /// Synonym for receiveAddress().
    Address to() const { return m_receiveAddress; }

    /// Synonym for safeSender().
    Address from() const { return safeSender(); }

    /// @returns the data associated with this (message-call) transaction. Synonym
    /// for initCode().
    bytes const& data() const { return m_data; }

    /// @returns the transaction-count of the sender.
    u256 nonce() const { return m_nonce; }

    /// Sets the nonce to the given value. Clears any signature.
    void setNonce(u256 const& _n)
    {
        clearSignature();
        m_nonce = _n;
        m_hashWith = h256(0);
        m_rlpBuffer = bytes();
    }

    void setBlockLimit(u256 const& _blockLimit)
    {
        clearSignature();
        m_blockLimit = _blockLimit;
        m_hashWith = h256(0);
        m_rlpBuffer = bytes();
    }

    /// @returns the latest block number to be packaged for transaction.
    u256 blockLimit() const { return m_blockLimit; }

    /// @returns the utc time at which a transaction enters the queue.
    u256 importTime() const { return m_importTime; }

    /// Sets the utc time at which a transaction enters the queue.
    void setImportTime(u256 _t) { m_importTime = _t; }

    /// @returns true if the transaction was signed
    bool hasSignature() const { return (bool)m_vrs; }

    /// @returns true if the transaction was signed with zero signature
    bool hasZeroSignature() const { return m_vrs && isZeroSignature(m_vrs->r, m_vrs->s); }

    u256 const& chainId() { return m_chainId; }
    u256 const& groupId() { return m_groupId; }
    dev::bytes const& extraData() { return m_extraData; }

    /// @returns the signature of the transaction (the signature has the sender
    /// encoded in it)
    /// @throws TransactionIsUnsigned if signature was not initialized
    std::shared_ptr<crypto::Signature> const& signature() const;
    void updateSignature(std::shared_ptr<crypto::Signature> sig)
    {
        m_vrs = sig;
        m_hashWith = h256(0);
        m_sender = Address();
        m_rlpBuffer = bytes();
    }
    /// @returns amount of gas required for the basic payment.
    int64_t baseGasRequired(EVMSchedule const& _es) const
    {
        return baseGasRequired(isCreation(), &m_data, _es);
    }

    /// Get the fee associated for a transaction with the given data.
    static int64_t baseGasRequired(
        bool _contractCreation, bytesConstRef _data, EVMSchedule const& _es);

    bool checkChainId(u256 _chainId);
    bool checkGroupId(u256 _groupId);

    void setRpcCallback(RPCCallback callBack);
    RPCCallback rpcCallback() const;

    void setRpcTx(bool const& _rpcTx) { m_rpcTx = _rpcTx; }
    bool rpcTx() { return m_rpcTx; }

    void setSynced(bool const& _synced) { m_synced = _synced; }
    bool synced() const { return m_synced; }

    int64_t capacity() { return (m_data.size() + m_rlpBuffer.size() + m_extraData.size()); }

    // Note: Provide for node transaction generation
    void setReceiveAddress(Address const& _receiveAddr)
    {
        m_hashWith = h256(0);
        m_rlpBuffer = bytes();
        m_receiveAddress = _receiveAddr;
    }
    void setData(std::shared_ptr<dev::bytes const> _dataPtr)
    {
        m_hashWith = h256(0);
        m_rlpBuffer = bytes();
        m_data = *_dataPtr;
    }

    void setChainId(u256 const& _chainId)
    {
        m_hashWith = h256(0);
        m_rlpBuffer = bytes();
        m_chainId = _chainId;
    }

    void setGroupId(u256 const& _groupId)
    {
        m_hashWith = h256(0);
        m_rlpBuffer = bytes();
        m_groupId = _groupId;
    }

    /// Type of transaction.
    enum Type
    {
        NullTransaction,   ///< Null transaction.
        ContractCreation,  ///< Transaction to create contracts - receiveAddress() is
                           ///< ignored.
        MessageCall        ///< Transaction to invoke a message call - receiveAddress() is
                           ///< used.
    };
    void setType(Type const& _type)
    {
        m_hashWith = h256(0);
        m_rlpBuffer = bytes();
        m_type = _type;
    }
    Type const& type() { return m_type; }

    void appendNodeContainsTransaction(dev::h512 const& _node)
    {
        return m_nodeTransactionMarker.appendNodeContainsTransaction(_node);
    }

    template <typename T>
    void appendNodeListContainTransaction(T const& _nodeList)
    {
        return m_nodeTransactionMarker.appendNodeListContainTransaction(_nodeList);
    }

    bool isTheNodeContainsTransaction(dev::h512 const& _node)
    {
        return m_nodeTransactionMarker.isTheNodeContainsTransaction(_node);
    }
    bool isKnownBySomeone() { return m_nodeTransactionMarker.isKnownBySomeone(); }

    void clearNodeTransactionMarker() { m_nodeTransactionMarker.clear(); }

    std::shared_ptr<crypto::Signature> vrs() { return m_vrs; }

protected:
    static bool isZeroSignature(u256 const& _r, u256 const& _s) { return !_r && !_s; }

    void encodeRC1(bytes& _trans, IncludeSignature _sig = WithSignature) const;
    void encodeRC2(bytes& _trans, IncludeSignature _sig = WithSignature) const;
    void decodeRC1(RLP const& rlp, CheckTransaction _checkSig = CheckTransaction::Everything);
    void decodeRC2(RLP const& rlp, CheckTransaction _checkSig = CheckTransaction::Everything);

    /// Clears the signature.
    void clearSignature()
    {
        m_vrs.reset();
        m_sender = Address();
    }

    Type m_type = NullTransaction;  ///< Is this a contract-creation transaction or
                                    ///< a message-call transaction?
    u256 m_nonce;                   ///< The transaction-count of the sender. Combined with
                                    ///< blockLimit for transaction de-duplication, and its
                                    ///< uniqueness is guaranteed by the client sending the
                                    ///< transaction.
    u256 m_value;                   ///< The amount of ETH to be transferred by this transaction.
                                    ///< Called 'endowment' for contract-creation transactions.
    Address m_receiveAddress;       ///< The receiving address of the transaction.
    u256 m_gasPrice;                ///< The base fee and thus the implied exchange rate of ETH
                                    ///< to GAS.
    u256 m_gas;    ///< The total gas to convert, paid for from sender's account. Any
                   ///< unused gas gets refunded once the contract is ended.
    bytes m_data;  ///< The data associated with the transaction, or the
                   ///< initialiser if it's a creation transaction.
    std::shared_ptr<crypto::Signature> m_vrs;  ///< The signature of the transaction.
                                               ///< Encodes the sender.
    mutable h256 m_hashWith;                   ///< Cached hash of transaction with signature.
    mutable Address m_sender;                  ///< Cached sender, determined from signature.
    u256 m_blockLimit;            ///< The latest block number to be packaged for transaction.
    u256 m_importTime = u256(0);  ///< The utc time at which a transaction enters the queue.

    RPCCallback m_rpcCallback;

    bytes m_rlpBuffer;  /// < The buffer to cache origin RLP sequence. It will be reused when the tx
                        /// < needs to be encocoded again;

    u256 m_chainId;     /// < The scenario to which the transaction belongs.
    u256 m_groupId;     /// < The group to which the transaction belongs.
    bytes m_extraData;  /// < Reserved fields, distinguished by "##".
    // used to represent that the transaction is received from rpc
    bool m_rpcTx = false;
    // Whether the transaction has been synchronized
    bool m_synced = false;
    // Record the list of nodes containing the transaction and provide related query interfaces.
    // This is separately abstracted as a class because the related map needs to be locked when
    // updating the node list, which makes the default copy constructor of Transaction invalid.
    NodeTransactionMarker m_nodeTransactionMarker;
};

/// Nice name for vector of Transaction.
using Transactions = std::vector<Transaction::Ptr>;

/// Simple human-readable stream-shift operator.
inline std::ostream& operator<<(std::ostream& _out, Transaction const& _t)
{
    _out << _t.hash().abridged() << "{";
    if (_t.receiveAddress())
        _out << _t.receiveAddress().abridged();
    else
        _out << "[CREATE]";

    _out << "/" << _t.data().size() << "$" << _t.value() << "+" << _t.gas() << "@" << _t.gasPrice();
    _out << "<-" << _t.safeSender().abridged() << " #" << _t.nonce() << "*" << _t.blockLimit()
         << "}";
    return _out;
}

class LocalisedTransaction
{
public:
    typedef std::shared_ptr<LocalisedTransaction> Ptr;

    LocalisedTransaction() {}
    LocalisedTransaction(
        h256 const& _blockHash, unsigned _transactionIndex, BlockNumber _blockNumber = 0)
      : m_tx(std::make_shared<Transaction>()),
        m_blockHash(_blockHash),
        m_transactionIndex(_transactionIndex),
        m_blockNumber(_blockNumber)
    {}

    LocalisedTransaction(Transaction::Ptr _tx, h256 const& _blockHash, unsigned _transactionIndex,
        BlockNumber _blockNumber = 0)
      : m_tx(_tx),
        m_blockHash(_blockHash),
        m_transactionIndex(_transactionIndex),
        m_blockNumber(_blockNumber)
    {}

    h256 const& blockHash() const { return m_blockHash; }
    unsigned transactionIndex() const { return m_transactionIndex; }
    BlockNumber blockNumber() const { return m_blockNumber; }
    Transaction::Ptr tx() { return m_tx; }

private:
    Transaction::Ptr m_tx;
    h256 m_blockHash;
    unsigned m_transactionIndex;
    BlockNumber m_blockNumber;
};

}  // namespace eth
}  // namespace dev
