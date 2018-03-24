// Copyright (c) 2014-2017 The D�sh Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//#define ENABLE_biblepay_DEBUG

#include "core_io.h"
#include "governance-classes.h"
#include "init.h"
#include "main.h"
#include "utilstrencodings.h"
#include "darksend.h"
#include "podc.h"
#include "masternode-payments.h"

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>

#include <univalue.h>

std::vector<std::string> Split(std::string s, std::string delim);
void GetDistributedComputingGovObjByHeight(int nHeight, uint256 uOptFilter, int& out_nVotes, uint256& out_uGovObjHash, std::string& out_PaymentAddresses, std::string& out_PaymentAmounts);
int GetRequiredQuorumLevel(int nHeight);


// DECLARE GLOBAL VARIABLES FOR GOVERNANCE CLASSES
CGovernanceTriggerManager triggerman;

// SPLIT UP STRING BY DELIMITER
// http://www.boost.org/doc/libs/1_58_0/doc/html/boost/algorithm/split_idp202406848.html
std::vector<std::string> SplitBy(std::string strCommand, std::string strDelimit)
{
    std::vector<std::string> vParts;
    boost::split(vParts, strCommand, boost::is_any_of(strDelimit));

    for(int q=0; q<(int)vParts.size(); q++) {
        if(strDelimit.find(vParts[q]) != std::string::npos) {
            vParts.erase(vParts.begin()+q);
            --q;
        }
    }

   return vParts;
}

CAmount ParsePaymentAmount(const std::string& strAmount)
{
    DBG( cout << "ParsePaymentAmount Start: strAmount = " << strAmount << endl; );

    CAmount nAmount = 0;
    if (strAmount.empty()) {
        std::ostringstream ostr;
        ostr << "ParsePaymentAmount: Amount is empty";
        throw std::runtime_error(ostr.str());
    }
    if(strAmount.size() > 20) {
        // String is much too long, the functions below impose stricter
        // requirements
        std::ostringstream ostr;
        ostr << "ParsePaymentAmount: Amount string too long";
        throw std::runtime_error(ostr.str());
    }
    // Make sure the string makes sense as an amount
    // Note: No spaces allowed
    // Also note: No scientific notation
    size_t pos = strAmount.find_first_not_of("0123456789.");
    if (pos != std::string::npos) {
        std::ostringstream ostr;
        ostr << "ParsePaymentAmount: Amount string contains invalid character";
        throw std::runtime_error(ostr.str());
    }

    pos = strAmount.find(".");
    if (pos == 0) {
        // JSON doesn't allow values to start with a decimal point
        std::ostringstream ostr;
        ostr << "ParsePaymentAmount: Invalid amount string, leading decimal point not allowed";
        throw std::runtime_error(ostr.str());
    }

    // Make sure there's no more than 1 decimal point
    if ((pos != std::string::npos) && (strAmount.find(".", pos+1) != std::string::npos)) {
        std::ostringstream ostr;
        ostr << "ParsePaymentAmount: Invalid amount string, too many decimal points";
        throw std::runtime_error(ostr.str());
    }

    // Note this code is taken from AmountFromValue in rpcserver.cpp
    // which is used for parsing the amounts in createrawtransaction.
    if (!ParseFixedPoint(strAmount, 8, &nAmount)) {
        nAmount = 0;
        std::ostringstream ostr;
        ostr << "ParsePaymentAmount: ParseFixedPoint failed for string: " << strAmount;
        throw std::runtime_error(ostr.str());
    }
    if (!MoneyRange(nAmount)) {
        nAmount = 0;
        std::ostringstream ostr;
        ostr << "ParsePaymentAmount: Invalid amount string, value outside of valid money range";
        throw std::runtime_error(ostr.str());
    }

    DBG( cout << "ParsePaymentAmount Returning true nAmount = " << nAmount << endl; );

    return nAmount;
}

/**
*   Add Governance Object
*/

bool CGovernanceTriggerManager::AddNewTrigger(uint256 nHash)
{
    DBG( cout << "CGovernanceTriggerManager::AddNewTrigger: Start" << endl; );
    AssertLockHeld(governance.cs);

    // IF WE ALREADY HAVE THIS HASH, RETURN
    if(mapTrigger.count(nHash)) {
        DBG(
            cout << "CGovernanceTriggerManager::AddNewTrigger: Already have hash"
                 << ", nHash = " << nHash.GetHex()
                 << ", count = " << mapTrigger.count(nHash)
                 << ", mapTrigger.size() = " << mapTrigger.size()
                 << endl; );
        return false;
    }

    CSuperblock_sptr pSuperblock;
    try  {
        CSuperblock_sptr pSuperblockTmp(new CSuperblock(nHash));
        pSuperblock = pSuperblockTmp;
    }
    catch(std::exception& e) {
        DBG( cout << "CGovernanceTriggerManager::AddNewTrigger Error creating superblock"
             << ", e.what() = " << e.what()
             << endl; );
        LogPrintf("CGovernanceTriggerManager::AddNewTrigger -- Error creating superblock: %s\n", e.what());
        return false;
    }
    catch(...) {
        LogPrintf("CGovernanceTriggerManager::AddNewTrigger: Unknown Error creating superblock\n");
        DBG( cout << "CGovernanceTriggerManager::AddNewTrigger Error creating superblock catchall" << endl; );
        return false;
    }

    pSuperblock->SetStatus(SEEN_OBJECT_IS_VALID);

    DBG( cout << "CGovernanceTriggerManager::AddNewTrigger: Inserting trigger" << endl; );
    mapTrigger.insert(std::make_pair(nHash, pSuperblock));

    DBG( cout << "CGovernanceTriggerManager::AddNewTrigger: End" << endl; );

    return true;
}

/**
*
*   Clean And Remove
*
*/

void CGovernanceTriggerManager::CleanAndRemove()
{
    LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- Start\n");
    DBG( cout << "CGovernanceTriggerManager::CleanAndRemove: Start" << endl; );
    AssertLockHeld(governance.cs);

    // LOOK AT THESE OBJECTS AND COMPILE A VALID LIST OF TRIGGERS
    for(trigger_m_it it = mapTrigger.begin(); it != mapTrigger.end(); ++it) {
        //int nNewStatus = -1;
        CGovernanceObject* pObj = governance.FindGovernanceObject((*it).first);
        if(!pObj) {
            continue;
        }
        CSuperblock_sptr& pSuperblock = it->second;
        if(!pSuperblock) {
            continue;
        }
        // IF THIS ISN'T A TRIGGER, WHY ARE WE HERE?
        if(pObj->GetObjectType() != GOVERNANCE_OBJECT_TRIGGER) {
            pSuperblock->SetStatus(SEEN_OBJECT_ERROR_INVALID);
        }
    }

    // Remove triggers that are invalid or already executed
    DBG( cout << "CGovernanceTriggerManager::CleanAndRemove: mapTrigger.size() = " << mapTrigger.size() << endl; );
    LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- mapTrigger.size() = %d\n", mapTrigger.size());
    trigger_m_it it = mapTrigger.begin();
    while(it != mapTrigger.end()) {
        bool remove = false;
        CSuperblock_sptr& pSuperblock = it->second;
        if(!pSuperblock) {
            DBG( cout << "CGovernanceTriggerManager::CleanAndRemove: NULL superblock marked for removal " << endl; );
            LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- NULL superblock marked for removal\n");
            remove = true;
        } else {
            DBG( cout << "CGovernanceTriggerManager::CleanAndRemove: superblock status = " << pSuperblock->GetStatus() << endl; );
            LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- superblock status = %d\n", pSuperblock->GetStatus());
            switch(pSuperblock->GetStatus()) {
            case SEEN_OBJECT_ERROR_INVALID:
            case SEEN_OBJECT_UNKNOWN:
                LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- Unknown or invalid trigger found\n");
                remove = true;
                break;
            case SEEN_OBJECT_IS_VALID:
            case SEEN_OBJECT_EXECUTED:
                {
                    int nTriggerBlock = pSuperblock->GetBlockStart();
                    // Rough approximation: a cycle of superblock ++
                    int nExpirationBlock = nTriggerBlock + GOVERNANCE_TRIGGER_EXPIRATION_BLOCKS;
                    LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- nTriggerBlock = %d, nExpirationBlock = %d\n", nTriggerBlock, nExpirationBlock);
                    if(governance.GetCachedBlockHeight() > nExpirationBlock) {
                        LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- Outdated trigger found\n");
                        remove = true;
                        CGovernanceObject* pgovobj = pSuperblock->GetGovernanceObject();
                        if(pgovobj) {
                            LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- Expiring outdated object: %s\n", pgovobj->GetHash().ToString());
                            pgovobj->fExpired = true;
                            pgovobj->nDeletionTime = GetAdjustedTime();
                        }
                    }
                }
                break;
            default:
                break;
            }
        }

        if(remove) {
            DBG(
                string strdata = "NULL";
                CGovernanceObject* pgovobj = pSuperblock->GetGovernanceObject();
                if(pgovobj) {
                    strdata = pgovobj->GetDataAsString();
                }
                cout << "CGovernanceTriggerManager::CleanAndRemove: Removing object: "
                     << strdata
                     << endl;
               );
            LogPrint("gobject", "CGovernanceTriggerManager::CleanAndRemove -- Removing trigger object\n");
            mapTrigger.erase(it++);
        }
        else  {
            ++it;
        }
    }

    DBG( cout << "CGovernanceTriggerManager::CleanAndRemove: End" << endl; );
}

/**
*   Get Active Triggers
*
*   - Look through triggers and scan for active ones
*   - Return the triggers in a list
*/

std::vector<CSuperblock_sptr> CGovernanceTriggerManager::GetActiveTriggers()
{
    AssertLockHeld(governance.cs);
    std::vector<CSuperblock_sptr> vecResults;

    DBG( cout << "GetActiveTriggers: mapTrigger.size() = " << mapTrigger.size() << endl; );

    // LOOK AT THESE OBJECTS AND COMPILE A VALID LIST OF TRIGGERS
    trigger_m_it it = mapTrigger.begin();
    while(it != mapTrigger.end()) {

        CGovernanceObject* pObj = governance.FindGovernanceObject((*it).first);

        if(pObj) {
            DBG( cout << "GetActiveTriggers: pObj->GetDataAsString() = " << pObj->GetDataAsString() << endl; );
            vecResults.push_back(it->second);
        }
        ++it;
    }

    DBG( cout << "GetActiveTriggers: vecResults.size() = " << vecResults.size() << endl; );

    return vecResults;
}

bool CSuperblockManager::IsDistributedComputingSuperblockTriggered(int nBlockHeight)
{
    LogPrint("gobject", "CSuperblockManager::IsDistributedComputingSuperblockTriggered -- Start nBlockHeight = %d\n", nBlockHeight);
    if (!CSuperblock::IsDCCSuperblock(nBlockHeight)) 
	{
        return false;
    }
	LOCK(governance.cs);
    // Check for Sanctuary Quorum Agreement 

	int iPendingVotes = 0;
	uint256 uGovObjHash;
	std::string sAddresses = "";
	std::string sAmounts = "";
	GetDistributedComputingGovObjByHeight(nBlockHeight, uint256S("0x0"), iPendingVotes, uGovObjHash, sAddresses, sAmounts);

	bool bPending = iPendingVotes >= GetRequiredQuorumLevel(nBlockHeight);
	if (!bPending)
	{
		LogPrintf("\n ** SUPERBLOCK DOES NOT HAVE ENOUGH VOTES : Required %f, Votes %f ", (double)GetRequiredQuorumLevel(nBlockHeight), (double)iPendingVotes);
		return false;
	}
	if (sAddresses.empty() || sAmounts.empty())
	{
		LogPrintf("\n ** SUPERBLOCK CONTRACT EMPTY at height %f ** \n",(double)nBlockHeight);
		return false;
	}

	if (fDebugMaster) LogPrint("podc", " ** IsDCCTriggered::Superblock has enough support - Votes %f  Addresses %s Amounts %s  **",(double)iPendingVotes, sAddresses.c_str(), sAmounts.c_str());
	return true;

}

/**
*   Is Superblock Triggered
*
*   - Does this block have a non-executed and actived trigger?
*/

bool CSuperblockManager::IsSuperblockTriggered(int nBlockHeight)
{
    LogPrint("gobject", "CSuperblockManager::IsSuperblockTriggered -- Start nBlockHeight = %d\n", nBlockHeight);
	if (CSuperblock::IsDCCSuperblock(nBlockHeight) && fDistributedComputingEnabled)
	{
		// The DCC Superblock is REQUIRED if it is voted and valid - but if it is not voted or not valid, it will become a regular block
		return IsDistributedComputingSuperblockTriggered(nBlockHeight);
	}

    if (!CSuperblock::IsValidBlockHeight(nBlockHeight)) 
	{
        return false;
    }

	
    LOCK(governance.cs);
    // GET ALL ACTIVE TRIGGERS
    std::vector<CSuperblock_sptr> vecTriggers = triggerman.GetActiveTriggers();

    LogPrint("gobject", "CSuperblockManager::IsSuperblockTriggered -- vecTriggers.size() = %d\n", vecTriggers.size());

    DBG( cout << "IsSuperblockTriggered Number triggers = " << vecTriggers.size() << endl; );

    BOOST_FOREACH(CSuperblock_sptr pSuperblock, vecTriggers)
    {
        if(!pSuperblock) {
            LogPrintf("CSuperblockManager::IsSuperblockTriggered -- Non-superblock found, continuing\n");
            DBG( cout << "IsSuperblockTriggered Not a superblock, continuing " << endl; );
            continue;
        }

        CGovernanceObject* pObj = pSuperblock->GetGovernanceObject();

        if(!pObj) {
            LogPrintf("CSuperblockManager::IsSuperblockTriggered -- pObj == NULL, continuing\n");
            DBG( cout << "IsSuperblockTriggered pObj is NULL, continuing" << endl; );
            continue;
        }

        LogPrint("gobject", "CSuperblockManager::IsSuperblockTriggered -- data = %s\n", pObj->GetDataAsString());

        // note : 12.1 - is epoch calculation correct?

        if(nBlockHeight != pSuperblock->GetBlockStart()) {
            LogPrintf("\n *****	CSuperblockManager::IsSuperblockTriggered -- block height doesn't match nBlockHeight = %d, blockStart = %d, continuing\n",
                     nBlockHeight,
                     pSuperblock->GetBlockStart());
            DBG( cout << "IsSuperblockTriggered Not the target block, continuing"
                 << ", nBlockHeight = " << nBlockHeight
                 << ", superblock->GetBlockStart() = " << pSuperblock->GetBlockStart()
                 << endl; );
            continue;
        }

        // MAKE SURE THIS TRIGGER IS ACTIVE VIA FUNDING CACHE FLAG

        pObj->UpdateSentinelVariables();

        if(pObj->IsSetCachedFunding()) 
		{
            LogPrint("gobject", "CSuperblockManager::IsSuperblockTriggered -- fCacheFunding = true, returning true\n");
            DBG( cout << "IsSuperblockTriggered returning true" << endl; );
            return true;
        }
        else  
		{
            LogPrint("gobject", "CSuperblockManager::IsSuperblockTriggered -- fCacheFunding = false, continuing\n");
            DBG( cout << "IsSuperblockTriggered No fCachedFunding, continuing" << endl; );
        }
		//	if (IsDistributedComputingSuperblockTriggered(nBlockHeight)) return true;  // Dont fail a DCC block for watchman-on-the-wall
    }

    return false;
}


bool CSuperblockManager::GetBestSuperblock(CSuperblock_sptr& pSuperblockRet, int nBlockHeight)
{
    if(!CSuperblock::IsValidBlockHeight(nBlockHeight) && !CSuperblock::IsDCCSuperblock(nBlockHeight)) 
	{
        return false;
    }

    AssertLockHeld(governance.cs);
    std::vector<CSuperblock_sptr> vecTriggers = triggerman.GetActiveTriggers();
    int nYesCount = 0;

    BOOST_FOREACH(CSuperblock_sptr pSuperblock, vecTriggers) {
        if(!pSuperblock) {
            DBG( cout << "GetBestSuperblock Not a superblock, continuing" << endl; );
            continue;
        }

        CGovernanceObject* pObj = pSuperblock->GetGovernanceObject();

        if(!pObj) {
            DBG( cout << "GetBestSuperblock pObj is NULL, continuing" << endl; );
            continue;
        }

        if(nBlockHeight != pSuperblock->GetBlockStart()) {
            DBG( cout << "GetBestSuperblock Not the target block, continuing" << endl; );
            continue;
        }

        // DO WE HAVE A NEW WINNER?

        int nTempYesCount = pObj->GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING);
        DBG( cout << "GetBestSuperblock nTempYesCount = " << nTempYesCount << endl; );
        if(nTempYesCount > nYesCount) {
            nYesCount = nTempYesCount;
            pSuperblockRet = pSuperblock;
            DBG( cout << "GetBestSuperblock Valid superblock found, pSuperblock set" << endl; );
        }
    }

    return nYesCount > 0;
}



/*
void CSuperblockManager::uperblock(CMutableTransaction& txNewRet, int nBlockHeight, std::vector<CTxOut>& voutSuperblockRet)
{

	if (!putingEnabled) return;
    LOCK(governance.cs);

    // Create the Distributed Computing Superblock For This Height - This happens Daily
    voutSuperblockRet.clear();
    // Superblock payments are appended to the end of the coinbase vout vector
    // TO DO: How many payments can we add before things blow up?  According to the Bitcoins largest transaction, about 32767 payments
	// (We should be OK for at least 5 years, then we will need to upgrade to multiple daily superblocks)
	CAmount nDCPaymentsTotal = CSuperblockLimit(nBlockHeight);
	int iPendingVotes = 0;
	uint256 uGovObjHash;
	vObjByHeight(nBlockHeight, uint256S("0x0"), iPendingVotes, uGovObjHash);

	bool bPending = iPendingVotes >= uorumLevel();
	LogPrintf(" Checking for Pending DCC - Votes %f ",(double)iPendingVotes);
	if (!bPending)
	{
		LogPrintf("\n ** uperblock::SUPERBLOCK DOES NOT HAVE ENOUGH VOTES : Required %f, Votes %f ", (double)uorumLevel(), (double)iPendingVotes);
		return;
	}
	std::string sContract = upcomingVote.contract;
	uint256 uHash = ash(sContract);
	double nTotalMagnitude = 0;
	int iCPIDCount = (sContract, nTotalMagnitude);
	if (nTotalMagnitude < .01) 
	{
		LogPrintf(" \n ** uperblock::SUPERBLOCK CONTAINS NO MAGNITUDE height %f (cpid count %f ), hash %s %s** \n", (double)nBlockHeight, 
			(double)iCPIDCount, uHash.GetHex().c_str(), sContract.c_str());
		return;
	}
	double dDCPaymentsTotal = nDCPaymentsTotal / COIN;
	if (dDCPaymentsTotal < 1)
	{
		LogPrintf(" \n ** uperblock::Superblock Payment Budget is lower than 1 BBP ** \n");
		return;
	}
	double PaymentPerMagnitude = (dDCPaymentsTotal-1) / nTotalMagnitude;
	std::vector<std::string> vRows = sContract.c_str(),"<ROW>");
	double dTotalPaid = 0;
	for (int i = 0; i < (int)vRows.size(); i++)
	{
		std::vector<std::string> vCPID = vRows[i].c_str(),",");
		if (vCPID.size() >= 3)
		{
			std::string sCpid = vCPID[1];
			std::string sAddress = vCPID[0];
			double dMagnitude = cdbl(vCPID[2],2);
			if (!sCpid.empty() && dMagnitude > 0)
			{
				double dOwed = PaymentPerMagnitude * dMagnitude;
				dTotalPaid += dOwed;
			    CBitcoinAddress cbaAddress(sAddress);
				CScript scriptPubKey = GetScriptForDestination(cbaAddress.Get());
     	        CTxOut txout = CTxOut(dOwed * COIN, scriptPubKey);
			    txNewRet.vout.push_back(txout);
	            voutSuperblockRet.push_back(txout);
	            // PRINT NICE LOG OUTPUT FOR SUPERBLOCK PAYMENT
                LogPrintf("uperblock::DCC Superblock : output %d (addr %s, amount %f)\n", i, sAddress.c_str(), (double)dOwed);
			}
		}
	}
	// Add SanctuaryQuorum Signatures
	
	std::string sSigs = mnpayments.SerializeSanctuaryQuorumSignatures(nBlockHeight, uHash);
	std::string sContractFinal = "<CONTRACT>" + sContract + "</CONTRACT>" + sSigs;
	int iStepLength = 990 - txNewRet.vout[0].sTxOutMessage.length();
	int iParts = cdbl(RoundToString(sContractFinal.length()/iStepLength,2),0);
	if (iParts < 1) iParts = 1;
	int iPtr = 0;
	for (int i = 0; i <= (int)sContractFinal.length(); i += iStepLength)
	{
		int iChunkLength = sContractFinal.length() - i;
		//if (iChunkLength <= iStepLength) bLastStep = true;
		if (iChunkLength > iStepLength) 
		{
			iChunkLength = iStepLength;
		} 
		std::string sChunk = sContractFinal.substr(i, iChunkLength);
		if ((iPtr+1) > (int)txNewRet.vout.size())
		{
			// Superblock too small to hold data
			CAmount nExtendedAmount = .000001 * COIN;
     	    CTxOut txoutExtension = CTxOut(nExtendedAmount, txNewRet.vout[iPtr].scriptPubKey);
			txNewRet.vout.push_back(txoutExtension);
			voutSuperblockRet.push_back(txoutExtension);
		}
		txNewRet.vout[iPtr].sTxOutMessage = sChunk;
		iPtr++;
	}


	// End of SanctuaryQuorum Signatures

	if (dTotalPaid > dDCPaymentsTotal)
	{
		LogPrintf("uperblock::DCC Superblock failure: Total Paid Available Daily Budget.  Creation Failed.  Paid %f - Budget %f\n", dTotalPaid, dDCPaymentsTotal);
        voutSuperblockRet.clear();
		return;
	}
	LogPrintf("\nuperblock::Success\n");
	return;

}


*/



/**
*   Create Superblock Payments
*
*   - Create the correct payment structure for a given superblock
*/

void CSuperblockManager::CreateSuperblock(CMutableTransaction& txNewRet, int nBlockHeight, std::vector<CTxOut>& voutSuperblockRet)
{
    DBG( cout << "CSuperblockManager::CreateSuperblock Start" << endl; );

    LOCK(governance.cs);

    // GET THE BEST SUPERBLOCK FOR THIS BLOCK HEIGHT

    CSuperblock_sptr pSuperblock;
    if(!CSuperblockManager::GetBestSuperblock(pSuperblock, nBlockHeight)) 
	{
        LogPrint("podc", "CSuperblockManager::CreateSuperblock -- Can't find superblock for height %d\n", nBlockHeight);
        if (fDebugMaster) 
		{
			DBG( cout << "CSuperblockManager::CreateSuperblock Failed to get superblock for height, returning" << endl; );
		}
        return;
    }

    // make sure it's empty, just in case
    voutSuperblockRet.clear();

    // CONFIGURE SUPERBLOCK OUTPUTS

    // Superblock payments are appended to the end of the coinbase vout vector
    if (fDebugMaster) 
	{
			DBG( cout << "CSuperblockManager::CreateSuperblock Number payments: " << pSuperblock->CountPayments() << endl; );
	}

    // TO DO: How many payments can we add before things blow up?
    //       Consider at least following limits:
    //          - max coinbase tx size
    //          - max "budget" available
	if (fDebugMaster) LogPrintf(" Creating superblock with %f payments \n", (double)pSuperblock->CountPayments());

    for(int i = 0; i < pSuperblock->CountPayments(); i++) {
        CGovernancePayment payment;
        DBG( cout << "CSuperblockManager::CreateSuperblock i = " << i << endl; );
        if(pSuperblock->GetPayment(i, payment)) 
		{
            DBG( cout << "CSuperblockManager::CreateSuperblock Payment found " << endl; );
            // SET COINBASE OUTPUT TO SUPERBLOCK SETTING

            CTxOut txout = CTxOut(payment.nAmount, payment.script);
            txNewRet.vout.push_back(txout);
            voutSuperblockRet.push_back(txout);

            // PRINT NICE LOG OUTPUT FOR SUPERBLOCK PAYMENT

            CTxDestination address1;
            ExtractDestination(payment.script, address1);
            CBitcoinAddress address2(address1);

            DBG( cout << "CSuperblockManager::CreateSuperblock Before LogPrintf call, nAmount = " << payment.nAmount << endl; );

            LogPrint("podc", "NEW Superblock : output %d (addr %s, amount %d)\n", i, address2.ToString(), payment.nAmount);
            
			DBG( cout << "CSuperblockManager::CreateSuperblock After LogPrintf call " << endl; );
        } 
		else 
		{
			DBG( cout << "CSuperblockManager::CreateSuperblock Payment not found " << endl; );
        }
    }

    DBG( cout << "CSuperblockManager::CreateSuperblock End" << endl; );
}

bool CSuperblockManager::IsValidSuperblock(const CTransaction& txNew, int nBlockHeight, CAmount blockReward, int64_t nBlockTime)
{
    // GET BEST SUPERBLOCK, SHOULD MATCH
    LOCK(governance.cs);
	// If this is a DistributedComputing Superblock, handle this block type:
	/*
	if (Enabled && CSuperblockCCSuperblock(nBlockHeight))
	{
		CSuperblock_sptr pSuperblock;
		uint256 nHash = uint256S("0x1");
        CSuperblock_sptr pSuperblockTmp(new CSuperblock(nHash));
    
		return pSuperblockTmp->IsValidSuperblock(txNew, nBlockHeight, blockReward, nBlockTime);
	}
	*/


    CSuperblock_sptr pSuperblock;
    if(CSuperblockManager::GetBestSuperblock(pSuperblock, nBlockHeight)) 
	{
        return pSuperblock->IsValidSuperblock(txNew, nBlockHeight, blockReward, nBlockTime);
    }

    return false;
}

CSuperblock::
CSuperblock()
    : nGovObjHash(),
      nEpochStart(0),
      nStatus(SEEN_OBJECT_UNKNOWN),
      vecPayments()
{}

CSuperblock::
CSuperblock(uint256& nHash)
    : nGovObjHash(nHash),
      nEpochStart(0),
      nStatus(SEEN_OBJECT_UNKNOWN),
      vecPayments()
{
    DBG( cout << "CSuperblock Constructor Start" << endl; );

    CGovernanceObject* pGovObj = GetGovernanceObject();

    if(!pGovObj) {
        DBG( cout << "CSuperblock Constructor pGovObjIn is NULL, returning" << endl; );
        throw std::runtime_error("CSuperblock: Failed to find Governance Object");
    }

    DBG( cout << "CSuperblock Constructor pGovObj : "
         << pGovObj->GetDataAsString()
         << ", nObjectType = " << pGovObj->GetObjectType()
         << endl; );

    if (pGovObj->GetObjectType() != GOVERNANCE_OBJECT_TRIGGER) {
        DBG( cout << "CSuperblock Constructor pHoObj not a trigger, returning" << endl; );
        throw std::runtime_error("CSuperblock: Governance Object not a trigger");
    }

    UniValue obj = pGovObj->GetJSONObject();

    // FIRST WE GET THE START EPOCH, THE DATE WHICH THE PAYMENT SHALL OCCUR
    nEpochStart = obj["event_block_height"].get_int();

    // NEXT WE GET THE PAYMENT INFORMATION AND RECONSTRUCT THE PAYMENT VECTOR
    std::string strAddresses = obj["payment_addresses"].get_str();
    std::string strAmounts = obj["payment_amounts"].get_str();
    ParsePaymentSchedule(strAddresses, strAmounts);

    LogPrint("gobject", "CSuperblock -- nEpochStart = %d, strAddresses = %s, strAmounts = %s, vecPayments.size() = %d\n",
             nEpochStart, strAddresses, strAmounts, vecPayments.size());

    DBG( cout << "CSuperblock Constructor End" << endl; );
}

/**
 *   Is Valid Superblock Height
 *
 *   - See if a block at this height can be a superblock
 */

bool CSuperblock::IsValidBlockHeight(int nBlockHeight)
{
    // SUPERBLOCKS CAN HAPPEN ONLY after hardfork and only ONCE PER CYCLE
    return nBlockHeight >= Params().GetConsensus().nSuperblockStartBlock &&
            ((nBlockHeight % Params().GetConsensus().nSuperblockCycle) == 0);
}

bool CSuperblock::IsDCCSuperblock(int nHeight)
{
	if (!fDistributedComputingEnabled) return false;
    return nHeight >= Params().GetConsensus().nDCCSuperblockStartBlock &&
            ((nHeight % Params().GetConsensus().nDCCSuperblockCycle) == 0);
}

CAmount CSuperblock::GetPaymentsLimit(int nBlockHeight)
{
	if (nBlockHeight < 10) return 0;

    const Consensus::Params& consensusParams = Params().GetConsensus();

    if ((!IsValidBlockHeight(nBlockHeight) && !IsDCCSuperblock(nBlockHeight))) 
	{
        return 0;
    }

    // min subsidy for high diff networks and vice versa
    int nBits = consensusParams.fPowAllowMinDifficultyBlocks ? UintToArith256(consensusParams.powLimit).GetCompact() : 1;

	bool fDCLive = (fProd && fDistributedComputingEnabled && nBlockHeight > F11000_CUTOVER_HEIGHT_PROD) || (!fProd && fDistributedComputingEnabled);

    // some part of all blocks issued during the cycle goes to superblock, see GetBlockSubsidy
	if (fDCLive) 
	{
		nBits = 486585255;  // Set diff at about 1.42 for Superblocks
	}
	int nSuperblockCycle = IsValidBlockHeight(nBlockHeight) ? consensusParams.nSuperblockCycle : consensusParams.nDCCSuperblockCycle;
	double nBudgetAvailable = (fDCLive && IsValidBlockHeight(nBlockHeight) && !IsDCCSuperblock(nBlockHeight)) ? .20 : 1;
    CAmount nSuperblockPartOfSubsidy = GetBlockSubsidy(pindexBestHeader->pprev, nBits, nBlockHeight, consensusParams, true);
    CAmount nPaymentsLimit = 0;
	if (nBudgetAvailable == 1)
	{
		nPaymentsLimit = nSuperblockPartOfSubsidy * nSuperblockCycle;  // Avoid floating point math problems here
	}
	else
	{
		nPaymentsLimit = nSuperblockPartOfSubsidy * nSuperblockCycle * nBudgetAvailable;
	}

	CAmount nAbsoluteMaxMonthlyBudget = 12500 * BLOCKS_PER_DAY * 30 * .20 * COIN; // Ensure monthly budget is never > 20% of avg monthly total block emission regardless of low difficulty in PODC
	// If this is a DC Superblock, and we exceed F12000 Cutover Height, due to cascading superblocks, the DC superblock budget should be 70% of the budget:
	if (IsDCCSuperblock(nBlockHeight) && nBlockHeight > F12000_CUTOVER_HEIGHT_PROD)
	{
		double nDCSuperblockBudget = .45;
		nPaymentsLimit = nPaymentsLimit * nDCSuperblockBudget;
	}

	if (nPaymentsLimit > nAbsoluteMaxMonthlyBudget) nPaymentsLimit = nAbsoluteMaxMonthlyBudget;

    LogPrint("gobject", "CSuperblock::GetPaymentsLimit -- Valid superblock height %f, payments max %f \n",(double)nBlockHeight, (double)nPaymentsLimit/COIN);

    return nPaymentsLimit;
}

void CSuperblock::ParsePaymentSchedule(std::string& strPaymentAddresses, std::string& strPaymentAmounts)
{
    // SPLIT UP ADDR/AMOUNT STRINGS AND PUT IN VECTORS

    std::vector<std::string> vecParsed1;
    std::vector<std::string> vecParsed2;
    vecParsed1 = SplitBy(strPaymentAddresses, "|");
    vecParsed2 = SplitBy(strPaymentAmounts, "|");

    // IF THESE DONT MATCH, SOMETHING IS WRONG

    if (vecParsed1.size() != vecParsed2.size()) {
        std::ostringstream ostr;
        ostr << "CSuperblock::ParsePaymentSchedule -- Mismatched payments and amounts";
        LogPrintf("%s\n", ostr.str());
        throw std::runtime_error(ostr.str());
    }

    if (vecParsed1.size() == 0) {
        std::ostringstream ostr;
        ostr << "CSuperblock::ParsePaymentSchedule -- Error no payments";
        LogPrintf("%s\n", ostr.str());
        throw std::runtime_error(ostr.str());
    }

    // LOOP THROUGH THE ADDRESSES/AMOUNTS AND CREATE PAYMENTS
    /*
      ADDRESSES = [ADDR1|2|3|4|5|6]
      AMOUNTS = [AMOUNT1|2|3|4|5|6]
    */

    DBG( cout << "CSuperblock::ParsePaymentSchedule vecParsed1.size() = " << vecParsed1.size() << endl; );

    for (int i = 0; i < (int)vecParsed1.size(); i++) {
        CBitcoinAddress address(vecParsed1[i]);
        if (!address.IsValid()) {
            std::ostringstream ostr;
            ostr << "CSuperblock::ParsePaymentSchedule -- Invalid Biblepay Address : " <<  vecParsed1[i];
            LogPrintf("%s\n", ostr.str());
            throw std::runtime_error(ostr.str());
        }

        DBG( cout << "CSuperblock::ParsePaymentSchedule i = " << i
             <<  ", vecParsed2[i] = " << vecParsed2[i]
             << endl; );

        CAmount nAmount = ParsePaymentAmount(vecParsed2[i]);

        DBG( cout << "CSuperblock::ParsePaymentSchedule: "
             << "amount string = " << vecParsed2[i]
             << ", nAmount = " << nAmount
             << endl; );

        CGovernancePayment payment(address, nAmount);
        if(payment.IsValid()) {
            vecPayments.push_back(payment);
        }
        else {
            vecPayments.clear();
            std::ostringstream ostr;
            ostr << "CSuperblock::ParsePaymentSchedule -- Invalid payment found: address = " << address.ToString()
                 << ", amount = " << nAmount;
            LogPrintf("%s\n", ostr.str());
            throw std::runtime_error(ostr.str());
        }
    }
}

bool CSuperblock::GetPayment(int nPaymentIndex, CGovernancePayment& paymentRet)
{
    if((nPaymentIndex<0) || (nPaymentIndex >= (int)vecPayments.size())) {
        return false;
    }

    paymentRet = vecPayments[nPaymentIndex];
    return true;
}

CAmount CSuperblock::GetPaymentsTotalAmount()
{
    CAmount nPaymentsTotalAmount = 0;
    int nPayments = CountPayments();

    for(int i = 0; i < nPayments; i++) {
        nPaymentsTotalAmount += vecPayments[i].nAmount;
    }

    return nPaymentsTotalAmount;
}


std::string CSuperblock::GetBlockData(const CTransaction& txNew)
{
	int nOutputs = txNew.vout.size();
	std::string sOut = "";
    for (int j = 0; j < nOutputs; j++) 
	{
		sOut += txNew.vout[j].sTxOutMessage;
	}
	return sOut;
}


/**
*   Is Transaction Valid
*
*   - Does this transaction match the superblock?
*/

bool CSuperblock::IsValidSuperblock(const CTransaction& txNew, int nBlockHeight, CAmount blockReward, int64_t nBlockTime)
{
    // TO DO : LOCK(cs);
    // No reason for a lock here now since this method only accesses data
    // internal to *this and since CSuperblock's are accessed only through
    // shared pointers there's no way our object can get deleted while this
    // code is running.
    if(!IsValidBlockHeight(nBlockHeight) && !IsDCCSuperblock(nBlockHeight))
	{
        LogPrintf("CSuperblock::IsValid -- ERROR: Block invalid, incorrect block height\n");
        return false;
    }

    std::string strPayeesPossible = "";

    // CONFIGURE SUPERBLOCK OUTPUTS

    int nOutputs = txNew.vout.size();
    int nPayments = CountPayments();
    int nMinerPayments = nOutputs - nPayments;

	if (IsValidBlockHeight(nBlockHeight))
	{
		LogPrint("gobject", "CSuperblock::IsValid nOutputs = %d, nPayments = %d, strData = %s\n",
             nOutputs, nPayments, GetGovernanceObject()->GetDataAsHex());
	}

    // We require an exact match (including order) between the expected
    // superblock payments and the payments actually in the block.

    if(nMinerPayments < 0) {
        // This means the block cannot have all the superblock payments
        // so it is not valid.
        // TO DO: could that be that we just hit coinbase size limit?
        LogPrintf("CSuperblock::IsValid -- ERROR: Block invalid, too few superblock payments\n");
        return false;
    }

    // payments should not exceed limit 2-5-2018
		
    CAmount nPaymentsTotalAmount = GetPaymentsTotalAmount();
    CAmount nPaymentsLimit = GetPaymentsLimit(nBlockHeight);
    if(nPaymentsTotalAmount > nPaymentsLimit) 
	{
        LogPrintf("\n\n ** CSuperblock::IsValid -- ERROR: Block invalid, payments limit exceeded: payments %f, limit %f ** \n", 
			(double)nPaymentsTotalAmount, (double)nPaymentsLimit);
        return false;
    }

    // miner should not get more than he would usually get
    CAmount nBlockValue = txNew.GetValueOut();
	if (IsValidBlockHeight(nBlockHeight))
	{
		if(nBlockValue > blockReward + nPaymentsTotalAmount) 
		{
			LogPrintf("CSuperblock::IsValid -- ERROR: Block invalid, block value limit exceeded: block %lld, limit %lld\n", (double)nBlockValue, (double)blockReward + nPaymentsTotalAmount);
			return false;
		}
	}

	// Robert A. - HANDLE BOTH MONTHLY AND DAILY SUPERBLOCKS:
	int nVoutIndex = 0;
	for(int i = 0; i < nPayments; i++) 
	{
			CGovernancePayment payment;
			if(!GetPayment(i, payment)) {
				// This shouldn't happen so log a warning
				LogPrintf("CSuperblock::IsValid -- WARNING: Failed to find payment: %d of %d total payments\n", i, nPayments);
				continue;
			}

			bool fPaymentMatch = false;

			for (int j = nVoutIndex; j < nOutputs; j++) {
				// Find superblock payment
				fPaymentMatch = ((payment.script == txNew.vout[j].scriptPubKey) &&
								 (payment.nAmount == txNew.vout[j].nValue));

				if (fPaymentMatch) {
					nVoutIndex = j;
					break;
				}
			}

			if(!fPaymentMatch) {
				// Superblock payment not found!

				CTxDestination address1;
				ExtractDestination(payment.script, address1);
				CBitcoinAddress address2(address1);
				LogPrintf("CSuperblock::IsValid -- ERROR: Block invalid: %d payment %d to %s not found\n", i, payment.nAmount, address2.ToString());

				return false;
			}
	}
	
	

	/*
		
		std::string sData = GetBlockData(txNew);
		std::string sContract = ExtractXML(sData,"<CONTRACT>","</CONTRACT>");
		std::string sSigs = ExtractXML(sData,"<SIGS>","</SIGS>");
		int64_t nAge = GetAdjustedTime() - nBlockTime;
		uint256 uHash = ash(sContract);
		// ENSURE DC RECIPIENTS MATCH SUPERBLOCK RECIPIENTS
		CAmount nDCPaymentsTotal = CSuperblock::(nBlockHeight);
		double dDCPaymentsTotal = nDCPaymentsTotal / COIN;
		double nTotalMagnitude = 0;
		int iCPIDCount = (sContract, nTotalMagnitude);
		double PaymentPerMagnitude = (dDCPaymentsTotal-1) / nTotalMagnitude;
		std::vector<std::string> vRows = sContract.c_str(),"<ROW>");
		LogPrintf(" ** IS VALID SUPERBLOCK:  Contract %s,   Sigs %s   \n  ", sContract.c_str(), sSigs.c_str());
		
		for (int i = 0; i < (int)vRows.size(); i++)
		{
			std::vector<std::string> vCPID = vRows[i].c_str(),",");
			if (vCPID.size() >= 3)
			{
				std::string sCpid = vCPID[1];
				std::string sAddress = vCPID[0];
				double dMagnitude = cdbl(vCPID[2],2);
				if (!sCpid.empty() && dMagnitude > 0)
				{
					double dOwed = PaymentPerMagnitude * dMagnitude;
				    CBitcoinAddress cbaAddress(sAddress);
					CScript scriptPubKey = GetScriptForDestination(cbaAddress.Get());
     				CAmount nAmount = dOwed * COIN;
					
					bool fPaymentMatch = false;
					for (int j = 0; j < nOutputs; j++) 
					{
						// Find superblock payment
						fPaymentMatch = ((scriptPubKey == txNew.vout[j].scriptPubKey) && (nAmount == txNew.vout[j].nValue));
						if (fPaymentMatch) 
						{
							LogPrintf(" VERIFY DCSUPERBLOCK - PAYMENT MATCH Address %s \n", sAddress.c_str());
							break;
						}
					}
					if (!fPaymentMatch) 
					{
						LogPrintf(" ISVALIDSUPERBLOCK::CANT FIND RESEARCHER PAYMENT FOR CPID %s, Amount %f, Address %s, cpidcount %f \n", sCpid.c_str(), (double)nAmount/COIN, sAddress.c_str(), (double)iCPIDCount);
						return false;
					}
				}
			}
		}
	
		if (nAge < 86400)
		{
			// Verify DC Hash matches todays SanctuaryQuorum Hash:
			CDistributedComputingVote upcomingVote;
			

			int iVotes = 0;
			uint256 uGovObjHash;
			vObjByHeight(nBlockHeight, uint256S("0x0"), iVotes, uGovObjHash);


			bool bPending = iVotes >= uorumLevel();
			LogPrintf(" ** VERIFY DCSUPERBLOCK - VOTES %f \n", (double)iVotes);

			if (!bPending)
			{
				LogPrintf("\n ** SUPERBLOCK DOES NOT HAVE ENOUGH VOTES : Required %f, Votes %f, ContractHash %s  ", 
					(double)uorumLevel(), (double)iVotes, uHash.GetHex().c_str());
				return false;
			}
	
		}

	}
	*/
	
	LogPrintf(" VERIFY DCSUPERBLOCK - ACCEPTED \n");
    return true;
}

/**
*   Get Required Payment String
*
*   - Get a string representing the payments required for a given superblock
*/

std::string CSuperblockManager::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(governance.cs);
    std::string ret = "Unknown";

    // GET BEST SUPERBLOCK

    CSuperblock_sptr pSuperblock;
    if(!GetBestSuperblock(pSuperblock, nBlockHeight)) {
        LogPrint("gobject", "CSuperblockManager::GetRequiredPaymentsString -- Can't find superblock for height %d\n", nBlockHeight);
        return "error";
    }

    // LOOP THROUGH SUPERBLOCK PAYMENTS, CONFIGURE OUTPUT STRING

    for(int i = 0; i < pSuperblock->CountPayments(); i++) {
        CGovernancePayment payment;
        if(pSuperblock->GetPayment(i, payment)) {
            // PRINT NICE LOG OUTPUT FOR SUPERBLOCK PAYMENT

            CTxDestination address1;
            ExtractDestination(payment.script, address1);
            CBitcoinAddress address2(address1);

            // RETURN NICE OUTPUT FOR CONSOLE

            if(ret != "Unknown") {
                ret += ", " + address2.ToString();
            }
            else {
                ret = address2.ToString();
            }
        }
    }

    return ret;
}
