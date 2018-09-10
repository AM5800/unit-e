#include <random>

// Copyright (c) 2018 The unit-e core developers
// Copyright (c) 2017-2018 The Particl Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <esperanza/finalizationstate.h>
#include <esperanza/kernel.h>
#include <esperanza/proposer.h>
#include <esperanza/stakevalidation.h>
#include <esperanza/walletextension.h>
#include <net.h>
#include <primitives/txtype.h>
#include <script/standard.h>
#include <utilfun.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>
#include <util.h>

namespace esperanza {

//! UNIT-E: check necessity of this constant
static const unsigned int DEFAULT_BLOCK_MAX_SIZE = 1000000;

WalletExtension::WalletExtension(::CWallet *enclosingWallet)
    : m_enclosingWallet(enclosingWallet) {
  assert(enclosingWallet != nullptr);
}

size_t WalletExtension::GetProposerThreadIndex() const {
  return m_stakeThreadIndex;
}

CAmount WalletExtension::GetStakeableBalance() const {
  LOCK2(cs_main, m_enclosingWallet->cs_wallet);

  CAmount balance = 0;

  for (const auto &it : m_enclosingWallet->mapWallet) {
    const CWalletTx &coin = it.second;
    if (coin.IsTrusted()) {
      balance += coin.GetAvailableCredit();
      balance += coin.GetAvailableWatchOnlyCredit();
    }
  }
  return balance;
}

void WalletExtension::AvailableCoinsForStaking(std::vector<::COutput> &vCoins) {
  vCoins.clear();

  // side effect: sets deepestTxnDepth - this is why this function is not
  // declared const (happens twice)
  m_deepestTxnDepth = 0;

  {
    LOCK2(cs_main, m_enclosingWallet->cs_wallet);

    int height = chainActive.Tip()->nHeight;
    int requiredDepth = std::min<int>(
        ::Params().GetEsperanza().GetStakeMinConfirmations() - 1,
        height / 2);

    for (const auto &it : m_enclosingWallet->mapWallet) {
      const CWalletTx &coin = it.second;
      CTransactionRef tx = coin.tx;

      int depth = coin.GetDepthInMainChain();  // requires cs_main lock

      if (depth > m_deepestTxnDepth) {
        // side effect: sets deepestTxnDepth - this is why this function is not
        // declared const (happens twice)
        m_deepestTxnDepth = depth;
      }
      if (depth < requiredDepth) {
        continue;
      }
      const uint256 &wtxid = it.first;
      const auto numOutputs = static_cast<const unsigned int>(tx->vout.size());
      for (unsigned int i = 0; i < numOutputs; ++i) {
        const auto &txout = tx->vout[i];

        COutPoint kernel(wtxid, i);
        if (!CheckStakeUnused(kernel) || m_enclosingWallet->IsSpent(wtxid, i) ||
            m_enclosingWallet->IsLockedCoin(wtxid, i)) {
          continue;
        }

        const CScript &pscriptPubKey = txout.scriptPubKey;
        CKeyID keyID;
        if (!ExtractStakingKeyID(pscriptPubKey, keyID)) {
          continue;
        }
        if (m_enclosingWallet->HaveKey(keyID)) {
          vCoins.emplace_back(&coin, i, depth,
                              /* fSpendable */ true, /* fSolvable */ true,
                              /* fSaveIn */ true);
        }
      }
    }
  }

  shuffle(vCoins.begin(), vCoins.end(), std::mt19937(std::random_device()()));
}

bool WalletExtension::SelectCoinsForStaking(
    int64_t nTargetValue, std::vector<::COutput> &availableCoinsForStaking,
    std::set<std::pair<const ::CWalletTx *, unsigned int>> &setCoinsRet,
    int64_t &nValueRet) {
  setCoinsRet.clear();
  nValueRet = 0;

  for (auto &output : availableCoinsForStaking) {
    const CWalletTx *pcoin = output.tx;
    int index = output.i;

    // Stop if we've chosen enough inputs
    if (nValueRet >= nTargetValue) {
      break;
    }

    int64_t amount = pcoin->tx->vout[index].nValue;

    std::pair<const CWalletTx *, unsigned int> coin =
        std::make_pair(pcoin, index);

    if (amount >= nTargetValue) {
      // If input value is greater or equal to target then simply insert
      //    it into the current subset and exit
      setCoinsRet.insert(coin);
      nValueRet += amount;
      break;
    } else if (amount < nTargetValue + EEES) {
      setCoinsRet.insert(coin);
      nValueRet += amount;
    }
  }

  return nValueRet >= nTargetValue;
}

bool WalletExtension::CreateCoinStake(unsigned int nBits, int64_t nTime,
                                      int nBlockHeight, int64_t nFees,
                                      ::CMutableTransaction &txNew,
                                      ::CKey &key) {
  CBlockIndex *pindexPrev = chainActive.Tip();
  arith_uint256 bnTargetPerCoinDay;
  bnTargetPerCoinDay.SetCompact(nBits);

  CAmount nBalance = GetStakeableBalance();
  if (nBalance <= m_reserveBalance) {
    return false;
  }

  // Choose coins to use
  std::vector<const CWalletTx *> vwtxPrev;
  std::set<std::pair<const CWalletTx *, unsigned int>> setCoins;
  CAmount nValueIn = 0;

  std::vector<::COutput> availableCoinsForStaking;
  AvailableCoinsForStaking(availableCoinsForStaking);
  if (!SelectCoinsForStaking(nBalance - m_reserveBalance,
                             availableCoinsForStaking, setCoins, nValueIn)) {
    return false;
  }

  CAmount nCredit = 0;
  CScript scriptPubKeyKernel;

  auto it = setCoins.begin();

  for (; it != setCoins.end(); ++it) {
    auto pcoin = *it;
    if (ProposerThread::IsStopped()) {
      return false;
    }
    COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);

    int64_t nBlockTime;
    if (CheckKernel(pindexPrev, nBits, nTime, prevoutStake, &nBlockTime)) {
      LOCK(m_enclosingWallet->cs_wallet);
      // Found a kernel
      LogPrint(BCLog::POS, "%s: Kernel found.\n", __func__);

      const CTxOut &kernelOut = pcoin.first->tx->vout[pcoin.second];

      std::vector<std::vector<unsigned char>> vSolutions;
      txnouttype whichType;

      CScript pscriptPubKey = kernelOut.scriptPubKey;
      CScript coinstakePath;
      bool fConditionalStake = false;
      if (HasIsCoinstakeOp(pscriptPubKey)) {
        fConditionalStake = true;
        if (!GetCoinstakeScriptPath(pscriptPubKey, coinstakePath)) {
          continue;
        }
        pscriptPubKey = coinstakePath;
      }

      if (!Solver(pscriptPubKey, whichType, vSolutions)) {
        LogPrint(BCLog::POS, "%s: Failed to parse kernel.\n", __func__);
        break;
      }

      LogPrint(BCLog::POS, "%s: Parsed kernel type=%d.\n", __func__, whichType);
      CKeyID spendId;
      if (whichType == TX_PUBKEYHASH) {
        spendId = CKeyID(uint160(vSolutions[0]));
      } else {
        LogPrint(BCLog::POS, "%s: No support for kernel type=%d.\n", __func__,
                 whichType);
        break;  // only support pay to address (pay to pubkey hash)
      }

      if (!m_enclosingWallet->GetKey(spendId, key)) {
        LogPrint(BCLog::POS, "%s: Failed to get key for kernel type=%d.\n",
                 __func__, whichType);
        break;  // unable to find corresponding key
      }

      if (fConditionalStake) {
        scriptPubKeyKernel = kernelOut.scriptPubKey;
      } else {
        scriptPubKeyKernel << OP_DUP << OP_HASH160 << ToByteVector(spendId)
                           << OP_EQUALVERIFY << OP_CHECKSIG;
      }

      // Ensure txn is empty
      txNew.vin.clear();
      txNew.vout.clear();

      // Mark as coin stake transaction
      txNew.SetVersion(1);  // UNIT-E: TODO decide transaction version
      txNew.SetType(TxType::COINSTAKE);

      txNew.vin.emplace_back(CTxIn(pcoin.first->GetHash(), pcoin.second));

      nCredit += kernelOut.nValue;
      vwtxPrev.push_back(pcoin.first);

      CTxOut out(0, scriptPubKeyKernel);
      txNew.vout.emplace_back(out);

      LogPrint(BCLog::POS, "%s: Added kernel.\n", __func__);

      setCoins.erase(it);
      break;
    }
  }

  if (nCredit == 0 || nCredit > nBalance - m_reserveBalance) {
    return false;
  }

  // Attempt to add more inputs
  // Only advantage here is to setup the next stake using this output as a
  // kernel to have a higher chance of staking
  size_t nStakesCombined = 0;
  it = setCoins.begin();
  while (it != setCoins.end()) {
    if (nStakesCombined >= m_maxStakeCombine) {
      break;
    }

    // Stop adding more inputs if already too many inputs
    if (txNew.vin.size() >= 100) {
      break;
    }

    // Stop adding more inputs if value is already pretty significant
    if (nCredit >= m_stakeCombineThreshold) {
      break;
    }

    auto itc = it++;  // copy the current iterator then increment it
    auto pcoin = *itc;

    const CTxOut &prevOut = pcoin.first->tx->vout[pcoin.second];

    // Only add coins of the same key/address as kernel
    if (prevOut.scriptPubKey != scriptPubKeyKernel) {
      continue;
    }
    // Stop adding inputs if reached reserve limit
    if (nCredit + prevOut.nValue > nBalance - m_reserveBalance) {
      break;
    }
    // Do not add additional significant input
    if (prevOut.nValue >= m_stakeCombineThreshold) {
      continue;
    }

    txNew.vin.emplace_back(pcoin.first->GetHash(), pcoin.second);
    nCredit += prevOut.nValue;
    vwtxPrev.push_back(pcoin.first);

    LogPrint(BCLog::POS, "%s: Combining kernel %s, %d.\n", __func__,
             pcoin.first->GetHash().ToString(), pcoin.second);
    nStakesCombined++;
    setCoins.erase(itc);
  }

  // Get block reward
  CAmount nReward =
      ::Params().GetEsperanza().GetProofOfStakeReward(pindexPrev, nFees);
  if (nReward < 0) {
    return false;
  }

  // Process development fund
  CAmount nRewardOut = nReward;

  // UNIT-E: Creating a reward to a rewardAddress has not been ported,
  // presumably belongs to coldstaking
  nCredit += nRewardOut;

  // Set output amount, split outputs if > nStakeSplitThreshold
  if (nCredit >= m_stakeSplitThreshold) {
    CTxOut outSplit(0, scriptPubKeyKernel);

    txNew.vout.back().nValue = nCredit / 2;
    outSplit.nValue = nCredit - txNew.vout.back().nValue;
    txNew.vout.emplace_back(outSplit);
  } else {
    txNew.vout.back().nValue = nCredit;
  }

  // Create output for reward
  // UNIT-E: Creating a reward to a rewardAddress has not been ported,
  // presumably belongs to coldstaking

  // Sign
  int nIn = 0;
  for (const auto &pcoin : vwtxPrev) {
    uint32_t nPrev = txNew.vin[nIn].prevout.n;

    const CTxOut &prevOut = pcoin->tx->vout[nPrev];
    const CScript &scriptPubKeyOut = prevOut.scriptPubKey;

    SignatureData sigdata;
    CTransaction txToConst(txNew);
    if (!ProduceSignature(
            TransactionSignatureCreator(m_enclosingWallet, &txToConst, nIn,
                                        prevOut.nValue, SIGHASH_ALL),
            scriptPubKeyOut, sigdata)) {
      return error("%s: ProduceSignature failed.", __func__);
    }

    UpdateTransaction(txNew, nIn, sigdata);
    nIn++;
  }

  // Limit size
  auto nBytes = static_cast<unsigned int>(
      ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION));
  if (nBytes >= DEFAULT_BLOCK_MAX_SIZE / 5) {
    return error("%s: Exceeded coinstake size limit.", __func__);
  }

  // Successfully generated coinstake
  return true;
}

bool WalletExtension::SignBlock(::CBlockTemplate *pblocktemplate, int nHeight,
                                int64_t nSearchTime) {
  // todo
  return false;
}

bool WalletExtension::SetMasterKeyFromSeed(const key::mnemonic::Seed &seed,
                                           std::string &error) {
  const std::string walletFileName = m_enclosingWallet->GetName();
  const std::time_t currentTime = std::time(nullptr);
  std::string backupWalletFileName =
      walletFileName + "~" + std::to_string(currentTime);
  m_enclosingWallet->BackupWallet(backupWalletFileName);
  const CPubKey hdMasterKey = m_enclosingWallet->GenerateNewHDMasterKey(&seed);
  if (!m_enclosingWallet->SetHDMasterKey(hdMasterKey)) {
    error = "setting master key failed";
    return false;
  }
  if (!m_enclosingWallet->NewKeyPool()) {
    error = "could not generate new keypool";
    return false;
  }
  return true;
}

// UNIT-E: read validatorState from the wallet file
void WalletExtension::ReadValidatorStateFromFile() {
  if (gArgs.GetBoolArg("-validating", false) && !gArgs.GetBoolArg("-staking", true)) {
    LogPrint(BCLog::ESPERANZA, "%s: -validating is enabled for wallet %s.\n", "ESPERANZA", m_enclosingWallet->GetName());

    validatorState = esperanza::ValidatorState();
    nIsValidatorEnabled = true;
  }
}

/**
 * Creates a deposit transaction from the given account and amount.
 *
 * @param[in] address the destination
 * @param[in] amount
 * @param[out] wtxOut the transaction created
 * @return true if the operation was succesful, false otherwise.
 */
bool WalletExtension::SendDeposit(const CTxDestination &address,
                                  const CAmount &amount,
                                  CWalletTx &wtxOut) {

  CCoinControl coinControl;
  CAmount nFeeRet;
  std::string sError;
  int nChangePosInOut = 1;
  std::vector<CRecipient> vecSend;

  CReserveKey reservekey(m_enclosingWallet);
  CKeyID keyID = boost::get<CKeyID>(address);
  CPubKey pubKey;
  m_enclosingWallet->GetPubKey(keyID, pubKey);

  CRecipient r{CScript::CreatePayVoteSlashScript(pubKey), amount, true};
  vecSend.push_back(r);

  if (!m_enclosingWallet->CreateTransaction(vecSend, wtxOut, reservekey,
                                            nFeeRet, nChangePosInOut, sError,
                                            coinControl, true, TxType::DEPOSIT)) {

    LogPrint(BCLog::ESPERANZA, "%s: Cannot create deposit transaction.\n", "ESPERANZA");
    return false;
  }

  CValidationState state;
  if (!m_enclosingWallet->CommitTransaction(wtxOut, reservekey, g_connman.get(), state)) {
    LogPrint(BCLog::ESPERANZA, "%s: Cannot commit deposit transaction.\n", "ESPERANZA");
    return false;
  }

  LogPrint(BCLog::ESPERANZA, "%s: Created new deposit transaction %s.\n", "ESPERANZA", wtxOut.GetHash().GetHex());

  {
    LOCK(m_enclosingWallet->cs_wallet);
    if (validatorState.m_phase == ValidatorState::ValidatorPhase::NOT_VALIDATING) {
      LogPrint(BCLog::ESPERANZA,
               "%s: Validator waiting for deposit confirmation.\n",
               "ESPERANZA");

      validatorState.m_phase = ValidatorState::ValidatorPhase::WAITING_DEPOSIT_CONFIRMATION;
    } else {
      LogPrintf(
          "ERROR: %s - Wrong state for validator state with deposit %s, %s "
          "expected.\n",
          "ESPERANZA", wtxOut.GetHash().GetHex(),
          "WAITING_DEPOSIT_CONFIRMATION");
    }
  }

  return true;
}

void WalletExtension::VoteIfNeeded(const std::shared_ptr<const CBlock> &pblock,
                                   const CBlockIndex *blockIndex) {

    esperanza::FinalizationState* esperanza = esperanza::FinalizationState::GetState(*blockIndex);
    uint32_t dynasty = esperanza->GetCurrentDynasty();

    if(dynasty > validatorState.m_endDynasty){
      return;
    }

    uint32_t epoch = esperanza::FinalizationState::GetEpoch(*blockIndex);

    //Avoid double votes
    if (validatorState.m_voteMap.find(epoch) != validatorState.m_voteMap.end()) {
      return;
    }

    LogPrint(BCLog::ESPERANZA, "%s: Validator voting for epoch %d and dynasty %d.\n",
             "ESPERANZA",
             epoch,
             dynasty);

    Vote vote = esperanza->GetRecommendedVote(validatorState.m_validatorIndex);

    //Check for sorrounding votes
    if (vote.m_targetEpoch < validatorState.m_lastTargetEpoch ||
        vote.m_sourceEpoch < validatorState.m_lastSourceEpoch) {
      return;
    }

    CWalletTx createdTx;
    CTransactionRef& prevRef = validatorState.m_lastVotableTx;

    if(SendVote(prevRef, vote, createdTx)){

      validatorState.m_voteMap[epoch] = vote;
      validatorState.m_lastTargetEpoch = vote.m_targetEpoch;
      validatorState.m_lastSourceEpoch = vote.m_sourceEpoch;
      validatorState.m_lastVotableTx = createdTx.tx;
      LogPrint(BCLog::ESPERANZA, "%s: Casted vote with id %s.\n",
               "ESPERANZA",
               createdTx.tx->GetHash().GetHex() );
    }
}

/**
 *
 * Creates a vote transaction starting from a Vote object and a previous
 * transaction (vote or deposit  reference. It fills inputs, outputs.
 * It does not support an address change between source and destination.
 *
 * @param[in] prevTx a reference to the initial DEPOSIT or previous VOTE transaction
 * @param[in] vote the vote data
 * @param[out] wtxNew the vote transaction committed
 */
bool WalletExtension::SendVote(const CTransactionRef &depositRef,
                               const Vote &vote, CWalletTx &wtxNewOut) {
    CCoinControl coinControl;
    wtxNewOut.fTimeReceivedIsTxTime = true;
    wtxNewOut.BindWallet(m_enclosingWallet);
    wtxNewOut.fFromMe = true;
    CReserveKey reservekey(m_enclosingWallet);
    CValidationState state;

    CMutableTransaction txNew;
    txNew.SetType(TxType::VOTE);

    if (validatorState.m_phase != ValidatorState::ValidatorPhase::IS_VALIDATING) {
      return error("%s: Cannot add vote inputs for non-validators.", __func__);
    }

    CScript scriptSig = CScript::EncodeVote(vote);

    const CScript &scriptPubKey = depositRef->vout[0].scriptPubKey;
    const CAmount amount = depositRef->vout[0].nValue;

    txNew.vin.push_back(CTxIn(depositRef->GetHash(), 0, scriptSig, CTxIn::SEQUENCE_FINAL));

    CTxOut txout(amount, scriptPubKey);
    txNew.vout.push_back(txout);

    CTransaction txNewConst(txNew);
    uint32_t nIn = 0;
    SignatureData sigdata;
    std::string strFailReason;

    if (!ProduceSignature(TransactionSignatureCreator(m_enclosingWallet, &txNewConst, nIn, amount, SIGHASH_ALL), scriptPubKey, sigdata, &txNewConst)) {
      strFailReason = _("Signing transaction failed");
      return false;
    } else {
      UpdateTransaction(txNew, nIn, sigdata);
    }

    // Embed the constructed transaction data in wtxNew.
    wtxNewOut.SetTx(MakeTransactionRef(std::move(txNew)));

    m_enclosingWallet->CommitTransaction(wtxNewOut, reservekey, g_connman.get(), state);
    if (state.IsInvalid()) {
      LogPrint(BCLog::ESPERANZA, "%s: Cannot commit vote transaction: %s.\n",
               "ESPERANZA", state.GetRejectReason());
      return false;
    }

  return true;
}

void WalletExtension::BlockConnected(const std::shared_ptr<const CBlock> &pblock, const CBlockIndex *pindex) {

  if (nIsValidatorEnabled && !IsInitialBlockDownload()) {
    ValidatorState::ValidatorPhase currentPhase;
    {
      LOCK(m_enclosingWallet->cs_wallet);
      currentPhase = validatorState.m_phase;
    }

    if (currentPhase == ValidatorState::ValidatorPhase::IS_VALIDATING) {
      VoteIfNeeded(pblock, pindex);
    } else if (currentPhase == ValidatorState::ValidatorPhase::WAITING_DEPOSIT_FINALIZATION) {
      FinalizationState *esperanza = FinalizationState::GetState(*pindex);

      if (esperanza->GetLastFinalizedEpoch() >= validatorState.m_depositEpoch) {
        // Deposit is finalized there is no possible rollback
        {
          LOCK(m_enclosingWallet->cs_wallet);
          validatorState.m_phase = ValidatorState::ValidatorPhase::IS_VALIDATING;

          LogPrint(
              BCLog::ESPERANZA,
              "%s: Validator's deposit finalized, the validator index is %s.\n",
              "ESPERANZA", validatorState.m_validatorIndex.GetHex());
        }
      }
    }
  }
}

}  // namespace esperanza