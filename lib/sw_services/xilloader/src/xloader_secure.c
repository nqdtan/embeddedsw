/******************************************************************************
* Copyright (c) 2019 - 2021 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

/*****************************************************************************/
/**
*
* @file xloader_secure.c
*
* This file contains all common security operations including sha related code
*
* <pre>
* MODIFICATION HISTORY:
*
* Ver   Who  Date     Changes
* ----- ---- -------- -------------------------------------------------------
* 1.00  vns  04/23/19 First release
* 1.01  vns  05/13/19 Added grey key decryption support
*       vns  06/14/19 Removed SHA padding related code
*       vns  07/09/19 Added PPK and SPK integrity checks
*                     Updated chunk size for secure partition
*                     Added encryption + authentication support
*       vns  07/23/19 Added functions to load secure headers
*       vns  08/23/19 Added buffer cleaning on failure
*                     Added different key sources support
*                     Added header decryption support
*                     Set hardware into reset upon failure
*       sb   08/24/19  Fixed coverity warnings
*       har  08/26/19 Fixed MISRA C violations
*       vns  08/28/19 Fixed bug in loading bigger secure CDOs
* 1.02  vns  02/23/20 Added DPA CM enable/disable functionality
*       vns  02/26/20 Added encryption revoke checks
*                     Added DEC_ONLY checks
*                     Updated PDI fields
*                     Added DPA CM enable/disable for MetaHeader
*       har  02/24/20 Added code to return error codes
*       rpo  02/25/20 Added SHA, RSA, ECDSA, AES KAT support
*       vns  03/01/20 Added PUF KEK decrypt support
*       ana  04/02/20 Added crypto engine KAT test function calls
*                     Removed release reset function calls from this file
*                     and added in respective library files
*       bsv  04/07/20 Change DMA name to PMCDMA
*       vns  04/13/20 Moved Aes instance to Secure structure
* 1.03  ana  06/04/20 Minor Enhancement and updated Sha3 hash buffer
*                     with XSecure_Sha3Hash Structure
*       tar  07/23/20 Fixed MISRA-C required violations
*       skd  07/29/20 Updated device copy macros
*       kpt  07/30/20 Added Meta header IV range checks and added IV
*                     support for ENC only case
*       kpt  08/01/20 Corrected check to validate the last row of ppk hash
*       bsv  08/06/20 Added delay load support for secure cases
*       kpt  08/10/20 Corrected endianness for meta header IV range checking
*       har  08/11/20 Added support for authenticated JTAG
*       td   08/19/20 Fixed MISRA C violations Rule 10.3
*       kal  08/23/20 Added parallel DMA support for Qspi and Ospi for secure
*       har  08/24/20 Added support for ECDSA P521 authentication
*       kpt  08/27/20 Changed argument type from u8* to UINTPTR for SHA
*       kpt  09/07/20 Fixed key rolling issue
*       kpt  09/08/20 Added redundancy at security critical checks
*       rpo  09/10/20 Added return type for XSecure_Sha3Start
*       bsv  09/30/20 Renamed XLOADER_CHUNK_MEMORY to XPLMI_PMCRAM_CHUNK_MEMORY
*       har  09/30/20 Deprecated Family Key support
*       bm   09/30/20 Added SecureClear API to clear security critical data
*                     in case of exceptions and also place AES, ECDSA_RSA,
*                     SHA3 in reset
*       kal  10/07/20 Added Missed DB check in XLoader_RsaSignVerify API
*       kal  10/16/20 Added a check for RSA EM MSB bit to make sure it is zero
*       kpt  10/19/20 Code clean up
*       td   10/19/20 MISRA C Fixes
*       bsv  10/19/20 Parallel DMA related changes
*       har  10/19/20 Replaced ECDSA in function calls
*       har  11/12/20 Initialized GVF in PufData structure with MSB of shutter
*                     value
*                     Improved checks for sync in PDI DPACM Cfg and Efuse DPACM Cfg
* 1.04  bm   12/16/20 Added PLM_SECURE_EXCLUDE macro. Also moved authentication and
*                     encryption related code to xloader_auth_enc.c file
*       bm   01/04/21 Updated checksum verification to be done at destination memory
*       kpt  02/18/21 Fixed logical error in partition next chunk copy in encryption cases
*
* </pre>
*
* @note
*
******************************************************************************/

/***************************** Include Files *********************************/
#include "xloader_secure.h"
#include "xloader_auth_enc.h"
#include "xilpdi.h"
#include "xplmi_dma.h"
#include "xsecure_error.h"
#include "xsecure_utils.h"
#include "xplmi.h"
#include "xplmi_modules.h"
#include "xplmi_scheduler.h"

/************************** Constant Definitions ****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/
#define XLOADER_SHA3_RESET_REG			(0xF1210004U)
#define XLOADER_SHA3_RESET_VAL			(0x1U)

/************************** Function Prototypes ******************************/
static int XLoader_StartNextChunkCopy(XLoader_SecureParams *SecurePtr,
	u32 TotalLen, u64 NextBlkAddr, u32 ChunkLen);
static int XLoader_ChecksumInit(XLoader_SecureParams *SecurePtr,
	const XilPdi_PrtnHdr *PrtnHdr);
static int XLoader_ProcessChecksumPrtn(XLoader_SecureParams *SecurePtr,
	u64 DestAddr, u32 BlockSize, u8 Last);
static int XLoader_VerifyHashNUpdateNext(XLoader_SecureParams *SecurePtr,
	u64 DataAddr, u32 Size, u8 Last);

/************************** Variable Definitions *****************************/

/************************** Function Definitions *****************************/

/*****************************************************************************/
/**
* @brief	This function initializes  XLoader_SecureParams's instance.
*
* @param	SecurePtr is pointer to the XLoader_SecureParams instance.
* @param	PdiPtr is pointer to the XilPdi instance
* @param	PrtnNum is the partition number to be processed
*
* @return	XST_SUCCESS on success and error code on failure
*
******************************************************************************/
int XLoader_SecureInit(XLoader_SecureParams *SecurePtr, XilPdi *PdiPtr,
	u32 PrtnNum)
{
	int Status = XST_FAILURE;
	XilPdi_PrtnHdr *PrtnHdr;

	Status = XPlmi_MemSetBytes(SecurePtr, sizeof(XLoader_SecureParams), 0U,
				sizeof(XLoader_SecureParams));
	if (Status != XST_SUCCESS) {
		Status = XPlmi_UpdateStatus(XLOADER_ERR_MEMSET,
				(int)XLOADER_ERR_MEMSET_SECURE_PTR);
		goto END;
	}

	/* Assign the partition header to local variable */
	PrtnHdr = &(PdiPtr->MetaHdr.PrtnHdr[PrtnNum]);
	SecurePtr->PdiPtr = PdiPtr;
	SecurePtr->ChunkAddr = XPLMI_PMCRAM_CHUNK_MEMORY;
	SecurePtr->BlockNum = 0x00U;
	SecurePtr->ProcessedLen = 0x00U;
	SecurePtr->PrtnHdr = PrtnHdr;

	/* Get DMA instance */
	SecurePtr->PmcDmaInstPtr = XPlmi_GetDmaInstance((u32)PMCDMA_0_DEVICE_ID);
	if (SecurePtr->PmcDmaInstPtr == NULL) {
		Status = XPlmi_UpdateStatus(XLOADER_ERR_INIT_GET_DMA, 0);
		goto END;
	}

	Status = XLoader_ChecksumInit(SecurePtr, PrtnHdr);
	if (Status != XST_SUCCESS) {
		goto END;
	}

#ifndef PLM_SECURE_EXCLUDE
	Status = XLoader_SecureAuthInit(SecurePtr, PrtnHdr);
	if (Status != XST_SUCCESS) {
		goto END;
	}

	Status = XLoader_SecureEncInit(SecurePtr, PrtnHdr);
	if (Status != XST_SUCCESS) {
		goto END;
	}
#endif

END:
	return Status;
}

/*****************************************************************************/
/**
* @brief	This function loads secure non-cdo partitions.
*
* @param	SecurePtr is pointer to the XLoader_SecureParams instance.
* @param	DestAddr is load address of the partition
* @param	Size is unencrypted size of the partition.
*
* @return	XST_SUCCESS on success and error code on failure
*
******************************************************************************/
int XLoader_SecureCopy(XLoader_SecureParams *SecurePtr, u64 DestAddr, u32 Size)
{
	int Status = XST_FAILURE;
	int ClrStatus = XST_FAILURE;
	u32 ChunkLen;
	u32 PdiVer;
	u32 Len = Size;
	u64 LoadAddr = DestAddr;
	u8 LastChunk = (u8)FALSE;

	PdiVer = SecurePtr->PdiPtr->MetaHdr.ImgHdrTbl.Version;
	if ((PdiVer != XLOADER_PDI_VERSION_1) &&
                (PdiVer != XLOADER_PDI_VERSION_2)) {
		ChunkLen = XLOADER_SECURE_CHUNK_SIZE;
	}
	else {
		ChunkLen = XLOADER_CHUNK_SIZE;
	}

	/*
	 * Double buffering is possible only
	 * when available PRAM Size >= ChunkLen * 2
	 */
	if ((SecurePtr->IsDoubleBuffering == (u8)TRUE) &&
		((ChunkLen * 2U) > XLOADER_CHUNK_SIZE)) {
		/*
		 * Blocking DMA will be used in case
		 * DoubleBuffering is FALSE.
		 */
		SecurePtr->IsDoubleBuffering = (u8)FALSE;
	}

	while (Len > 0U) {
		/* Update the length for last chunk */
		if (Len <= ChunkLen) {
			LastChunk = (u8)TRUE;
			ChunkLen = Len;
		}

		SecurePtr->RemainingDataLen = Len;

		/* Call security function */
		Status = XLoader_ProcessSecurePrtn(SecurePtr, LoadAddr,
					ChunkLen, LastChunk);
		if (Status != XST_SUCCESS) {
			goto END;
		}

		/* Update variables for next chunk */
		LoadAddr = LoadAddr + SecurePtr->SecureDataLen;
		Len = Len - SecurePtr->ProcessedLen;

		if (SecurePtr->IsDoubleBuffering == (u8)TRUE) {
			SecurePtr->ChunkAddr = SecurePtr->NextChunkAddr;
		}
	}

END:
	if (Status != XST_SUCCESS) {
		/* On failure clear data at destination address */
		ClrStatus = XPlmi_InitNVerifyMem(DestAddr, Size);
		if (ClrStatus != XST_SUCCESS) {
			Status = (int)((u32)Status | XLOADER_SEC_BUF_CLEAR_ERR);
		}
		else {
			Status = (int)((u32)Status | XLOADER_SEC_BUF_CLEAR_SUCCESS);
		}
	}
	return Status;
}

/*****************************************************************************/
/**
* @brief	This function performs authentication, checksum and decryption
* of the partition.
*
* @param	SecurePtr is pointer to the XLoader_SecureParams instance
* @param	DestAddr is the address to which data is copied
* @param	BlockSize is size of the data block to be processed
*		which doesn't include padding lengths and hash.
* @param	Last notifies if the block to be processed is last or not
*
* @return	XST_SUCCESS on success and error code on failure
*
******************************************************************************/
int XLoader_ProcessSecurePrtn(XLoader_SecureParams *SecurePtr, u64 DestAddr,
				u32 BlockSize, u8 Last)
{
	int Status = XST_FAILURE;

	if (SecurePtr->IsCheckSumEnabled == (u8)TRUE) {
		Status = XLoader_ProcessChecksumPrtn(SecurePtr, DestAddr,
				BlockSize, Last);
	}
#ifndef PLM_SECURE_EXCLUDE
	else if ((SecurePtr->IsAuthenticated == (u8)TRUE) ||
		(SecurePtr->IsEncrypted == (u8)TRUE)) {
		Status = XLoader_ProcessAuthEncPrtn(SecurePtr, DestAddr,
				BlockSize, Last);
	}
#endif
	else {
		/* For Misra-C */
	}

	return Status;
}

/*****************************************************************************/
/**
* @brief	This function starts next chunk copy when security is enabled.
*
* @param	SecurePtr is pointer to the XLoader_SecureParams instance.
* @param	TotalLen is total length of the partition.
* @param	NextBlkAddr is the address of the next chunk data to be copied.
* @param 	ChunkLen is size of the data block to be copied.
*
* @return	XST_SUCCESS on success and error code on failure
*
******************************************************************************/
static int XLoader_StartNextChunkCopy(XLoader_SecureParams *SecurePtr,
		u32 TotalLen, u64 NextBlkAddr, u32 ChunkLen)
{
	int Status = XST_FAILURE;
	u32 CopyLen = ChunkLen;

	if (SecurePtr->ChunkAddr == XPLMI_PMCRAM_CHUNK_MEMORY) {
		SecurePtr->NextChunkAddr = XPLMI_PMCRAM_CHUNK_MEMORY_1;
	}
	else {
		SecurePtr->NextChunkAddr = XPLMI_PMCRAM_CHUNK_MEMORY;
	}

	if (TotalLen <= ChunkLen) {
		CopyLen = TotalLen;
	}
	else {
		#ifndef PLM_SECURE_EXCLUDE
		if ((SecurePtr->IsAuthenticated == (u8)TRUE) ||
			(SecurePtr->IsCheckSumEnabled == (u8)TRUE)) {
			CopyLen = CopyLen + XLOADER_SHA3_LEN;
		}
		#else
		if (SecurePtr->IsCheckSumEnabled == (u8)TRUE) {
			CopyLen = CopyLen + XLOADER_SHA3_LEN;
		}
		#endif
	}

	SecurePtr->IsNextChunkCopyStarted = (u8)TRUE;

	/* Initiate the data copy */
	Status = SecurePtr->PdiPtr->DeviceCopy(NextBlkAddr,
			SecurePtr->NextChunkAddr, CopyLen,
			XPLMI_DEVICE_COPY_STATE_INITIATE);
	if (Status != XST_SUCCESS) {
		Status = XPlmi_UpdateStatus(XLOADER_ERR_DATA_COPY_FAIL,
				Status);
	}

	return Status;
}

/*****************************************************************************/
/**
 * @brief	This function is called to clear secure critical data in case of
 * exceptions. The function also places AES, ECDSA_RSA and SHA3 in reset.
 *
 * @param	None
 *
 * @return	None
 *
 *****************************************************************************/
void XLoader_SecureClear(void)
{
#ifndef PLM_SECURE_EXCLUDE
	XLoader_AuthEncClear();
#endif
	/* Place SHA3 in reset */
	XPlmi_Out32(XLOADER_SHA3_RESET_REG, XLOADER_SHA3_RESET_VAL);
}

/*****************************************************************************/
/**
* @brief	This function calculates hash and compares with expected hash.
* For every block, hash of next block is updated into expected hash.
*
* @param	SecurePtr is pointer to the XLoader_SecureParams instance.
* @param	DataAddr is the address of the data present in the block
* @param	Size is size of the data block to be processed
*		which includes padding lengths and hash.
* @param	Last notifies if the block to be processed is last or not.
*
* @return	XST_SUCCESS on success and error code on failure
*
******************************************************************************/
static int XLoader_VerifyHashNUpdateNext(XLoader_SecureParams *SecurePtr,
	u64 DataAddr, u32 Size, u8 Last)
{
	volatile int Status = XST_FAILURE;
	XSecure_Sha3 Sha3Instance;
	XSecure_Sha3Hash BlkHash = {0U};
	u8 *ExpHash = (u8 *)SecurePtr->Sha3Hash;

	if (SecurePtr->PmcDmaInstPtr == NULL) {
		goto END;
	}

	Status = XSecure_Sha3Initialize(&Sha3Instance, SecurePtr->PmcDmaInstPtr);
	if (Status != XST_SUCCESS) {
		Status = XPlmi_UpdateStatus(XLOADER_ERR_PRTN_HASH_CALC_FAIL,
				Status);
		goto END;
	}

	Status = XSecure_Sha3Start(&Sha3Instance);
	if (Status != XST_SUCCESS) {
		Status = XPlmi_UpdateStatus(XLOADER_ERR_PRTN_HASH_CALC_FAIL,
			Status);
		goto END;
	}

	/* Update next chunk's hash from pmc ram */
	if ((Last != (u8)TRUE) && (SecurePtr->IsCdo != (u8)TRUE)) {
		Status = XSecure_Sha3Update64Bit(&Sha3Instance,
				(u64)SecurePtr->ChunkAddr, XLOADER_SHA3_LEN);
		if (Status != XST_SUCCESS) {
			Status = XPlmi_UpdateStatus(XLOADER_ERR_PRTN_HASH_CALC_FAIL, Status);
			goto END;
		}
	}

	Status = XSecure_Sha3Update64Bit(&Sha3Instance, DataAddr, Size);
	if (Status != XST_SUCCESS) {
		Status = XPlmi_UpdateStatus(XLOADER_ERR_PRTN_HASH_CALC_FAIL, Status);
		goto END;
	}

	Status = XSecure_Sha3Finish(&Sha3Instance, &BlkHash);
	if (Status != XST_SUCCESS) {
		Status = XPlmi_UpdateStatus(XLOADER_ERR_PRTN_HASH_CALC_FAIL, Status);
		goto END;
	}

	Status = Xil_MemCmp(ExpHash, BlkHash.Hash, XLOADER_SHA3_LEN);
	if (Status != XST_SUCCESS) {
		XPlmi_Printf(DEBUG_INFO, "Hash mismatch error\n\r");
		XPlmi_PrintArray(DEBUG_INFO, (UINTPTR)BlkHash.Hash,
			XLOADER_SHA3_LEN / XIH_PRTN_WORD_LEN, "Calculated Hash");
		XPlmi_PrintArray(DEBUG_INFO, (UINTPTR)ExpHash,
			XLOADER_SHA3_LEN / XIH_PRTN_WORD_LEN, "Expected Hash");
		Status = XPlmi_UpdateStatus(XLOADER_ERR_PRTN_HASH_COMPARE_FAIL,
			Status);
		goto END;
	}

	/* Update the next expected hash  and data location */
	if (Last != (u8)TRUE) {
		Status = Xil_SecureMemCpy(ExpHash, XLOADER_SHA3_LEN,
					(u8 *)SecurePtr->ChunkAddr, XLOADER_SHA3_LEN);
		if (Status != XST_SUCCESS) {
			goto END;
		}
	}

END:
	return Status;
}

/*****************************************************************************/
/**
* @brief       This function initializes checksum parameters of
* XLoader_SecureParams's instance
*
* @param	SecurePtr is pointer to the XLoader_SecureParams instance.
* @param	PrtnHdr is pointer to XilPdi_PrtnHdr instance
*
* @return	XST_SUCCESS on success and error code on failure
*
******************************************************************************/
static int XLoader_ChecksumInit(XLoader_SecureParams *SecurePtr,
			const XilPdi_PrtnHdr *PrtnHdr)
{
	int Status = XST_FAILURE;
	u32 ChecksumType;
	u64 ChecksumOffset;

	ChecksumType = XilPdi_GetChecksumType(PrtnHdr);
	/* Check if checksum is enabled */
	if (ChecksumType != 0x00U) {
		 XPlmi_Printf(DEBUG_INFO,
			 "Checksum verification is enabled\n\r");

		/* Check checksum type */
		if(ChecksumType == XIH_PH_ATTRB_HASH_SHA3) {
			SecurePtr->IsCheckSumEnabled = (u8)TRUE;
			SecurePtr->SecureEn = (u8)TRUE;
			SecurePtr->SecureEnTmp = (u8)TRUE;
		}
		else {
			/* Only SHA3 checksum is supported */
			Status = XPlmi_UpdateStatus(
				XLOADER_ERR_INIT_INVALID_CHECKSUM_TYPE, 0);
			goto END;
		}

		/* Copy checksum hash */
		if (SecurePtr->PdiPtr->PdiType == XLOADER_PDI_TYPE_RESTORE) {
			Status = SecurePtr->PdiPtr->DeviceCopy(
					SecurePtr->PdiPtr->CopyToMemAddr,
					(UINTPTR)SecurePtr->Sha3Hash, XLOADER_SHA3_LEN, 0U);
			SecurePtr->PdiPtr->CopyToMemAddr += XLOADER_SHA3_LEN;
		}
		else {
			ChecksumOffset = SecurePtr->PdiPtr->MetaHdr.FlashOfstAddr +
					((u64)SecurePtr->PrtnHdr->ChecksumWordOfst *
						XIH_PRTN_WORD_LEN);
			if (SecurePtr->PdiPtr->CopyToMem == (u8)TRUE) {
				Status = SecurePtr->PdiPtr->DeviceCopy(ChecksumOffset,
						SecurePtr->PdiPtr->CopyToMemAddr,
						XLOADER_SHA3_LEN, 0U);
				SecurePtr->PdiPtr->CopyToMemAddr += XLOADER_SHA3_LEN;
			}
			else {
				Status = SecurePtr->PdiPtr->DeviceCopy(ChecksumOffset,
					(UINTPTR)SecurePtr->Sha3Hash, XLOADER_SHA3_LEN, 0U);
			}
		}
		if (Status != XST_SUCCESS){
			Status = XPlmi_UpdateStatus(
				XLOADER_ERR_INIT_CHECKSUM_COPY_FAIL, Status);
			goto END;
		}
		SecurePtr->SecureHdrLen += XLOADER_SHA3_LEN;
	}

	Status = XST_SUCCESS;

END:
	return Status;
}

/*****************************************************************************/
/**
* @brief	This function performs checksum of the partition.
*
* @param	SecurePtr is pointer to the XLoader_SecureParams instance
* @param	DestAddr is the address to which data is copied
* @param	BlockSize is size of the data block to be processed
*		which doesn't include padding lengths and hash.
* @param	Last notifies if the block to be processed is last or not
*
* @return	XST_SUCCESS on success and error code on failure
*
******************************************************************************/
static int XLoader_ProcessChecksumPrtn(XLoader_SecureParams *SecurePtr,
	u64 DestAddr, u32 BlockSize, u8 Last)
{

	volatile int Status = XST_FAILURE;
	u32 TotalSize = BlockSize;
	u64 SrcAddr;

	XPlmi_Printf(DEBUG_DETAILED,
			"Processing Block %u \n\r", SecurePtr->BlockNum);
	SecurePtr->ProcessedLen = 0U;
	/* 1st block */
	if (SecurePtr->BlockNum == 0x0U) {
		SrcAddr = SecurePtr->PdiPtr->MetaHdr.FlashOfstAddr +
				((u64)(SecurePtr->PrtnHdr->DataWordOfst) * XIH_PRTN_WORD_LEN);
	}
	else {
		SrcAddr = SecurePtr->NextBlkAddr;
	}

	/*
	* Except for the last block of data,
	* SHA3 hash(48 bytes) of next block should
	* be added for block size
	*/
	if (Last != (u8)TRUE) {
		TotalSize = TotalSize + XLOADER_SHA3_LEN;
	}

	Status = XLoader_SecureChunkCopy(SecurePtr, SrcAddr, Last,
				BlockSize, TotalSize);
	if (Status != XST_SUCCESS) {
		goto END;
	}

	if (SecurePtr->IsCdo == (u8)TRUE) {
		/* Verify hash */
		XSECURE_TEMPORAL_CHECK(END, Status, XLoader_VerifyHashNUpdateNext,
			SecurePtr, SecurePtr->ChunkAddr, TotalSize, Last);
	}

	if (Last != (u8)TRUE) {
		/* Here Authentication overhead is removed in the chunk */
		SecurePtr->SecureData = SecurePtr->ChunkAddr + XLOADER_SHA3_LEN;
		SecurePtr->SecureDataLen = TotalSize - XLOADER_SHA3_LEN;
	}
	else {
		/* This is the last block */
		SecurePtr->SecureData = SecurePtr->ChunkAddr;
		SecurePtr->SecureDataLen = TotalSize;
	}

	if (SecurePtr->IsCdo != (u8)TRUE) {
			/* Copy to destination address */
		Status = XPlmi_DmaXfr((u64)SecurePtr->SecureData, (u64)DestAddr,
				SecurePtr->SecureDataLen / XIH_PRTN_WORD_LEN,
				XPLMI_PMCDMA_0);
		if (Status != XST_SUCCESS) {
			Status = XPlmi_UpdateStatus(
					XLOADER_ERR_DMA_TRANSFER, Status);
			goto END;
		}

		XSECURE_TEMPORAL_CHECK(END, Status, XLoader_VerifyHashNUpdateNext,
			SecurePtr, DestAddr, SecurePtr->SecureDataLen, Last);
	}

	SecurePtr->NextBlkAddr = SrcAddr + TotalSize;
	SecurePtr->ProcessedLen = TotalSize;
	SecurePtr->BlockNum++;

END:
	return Status;
}

/*****************************************************************************/
/**
* @brief        This function copies the data from SrcAddr to chunk memory during
*		processing of secure partitions
*
* @param	SecurePtr is pointer to the XLoader_SecureParams instance
* @param	SrcAddr is the source address from which the data is to be
* 		processed or copied
* @param	Last notifies if the block to be processed is last or not
* @param	BlockSize is size of the data block to be processed
*		which doesn't include padding lengths and hash.
* @param	TotalSize is pointer to TotalSize which has to be processed
*
* @return	XST_SUCCESS on success and error code on failure
*
******************************************************************************/
int XLoader_SecureChunkCopy(XLoader_SecureParams *SecurePtr, u64 SrcAddr,
			u8 Last, u32 BlockSize, u32 TotalSize)
{
	int Status = XST_FAILURE;

	if (SecurePtr->IsNextChunkCopyStarted == (u8)TRUE) {
		SecurePtr->IsNextChunkCopyStarted = (u8)FALSE;
		/* Wait for copy to get completed */
		Status = SecurePtr->PdiPtr->DeviceCopy(SrcAddr,
					SecurePtr->ChunkAddr, TotalSize,
					XPLMI_DEVICE_COPY_STATE_WAIT_DONE);
	}
	else {
		/* Copy the data to PRAM buffer */
		Status = SecurePtr->PdiPtr->DeviceCopy(SrcAddr,
					SecurePtr->ChunkAddr, TotalSize,
					XPLMI_DEVICE_COPY_STATE_BLK);
	}
	if (Status != XST_SUCCESS) {
		Status = XPlmi_UpdateStatus(
				XLOADER_ERR_DATA_COPY_FAIL, Status);
		goto END;
	}

	if ((SecurePtr->IsDoubleBuffering == (u8)TRUE) &&
				(Last != (u8)TRUE)) {
		Status = XLoader_StartNextChunkCopy(SecurePtr,
					(SecurePtr->RemainingDataLen - TotalSize),
					SrcAddr + TotalSize, BlockSize);
		if (Status != XST_SUCCESS) {
			goto END;
		}
	}
END:
	return Status;
}
