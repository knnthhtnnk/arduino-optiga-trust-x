/*
 * MIT License
 *
 * Copyright (c) 2018 Infineon Technologies AG
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE
 *
 * Arduino library for OPTIGA™ Trust X.
 */
#include "OPTIGATrustX.h"
#include "optiga_trustx/CommandLib.h"
#include "optiga_trustx/IntegrationLib.h"
#include "optiga_trustx/optiga_comms.h"
#include "optiga_trustx/ifx_i2c_config.h"
#include "optiga_trustx/pal_os_event.h"
#include "third_crypto/uECC.h"
#include "aes/AES.h"

///OID of IFX Certificate
#define     OID_IFX_CERTIFICATE                 0xE0E0
///OID of the Coprocessor UID
#define     OID_IFX_UID                         0xE0C2
#define     LENGTH_UID                          27
///Length of certificate
#define     LENGTH_CERTIFICATE                  1728
///ASN Tag for sequence
#define     ASN_TAG_SEQUENCE                    0x30
///ASN Tag for integer
#define     ASN_TAG_INTEGER                     0x02
///msb bit mask
#define     MASK_MSB                            0x80
///TLS Identity Tag
#define     TLS_TAG                             0xC0
///IFX Private Key Slot
#define     OID_PRIVATE_KEY                     0xE0F0
///Power Limit OID
#define     OID_CURRENT_LIMIT                   0xE0C4
///Length of R and S vector
#define     LENGTH_RS_VECTOR                    0x40

///Length of maximum additional bytes to encode sign in DER
#define     MAXLENGTH_SIGN_ENCODE               0x08

///Length of Signature
#define     LENGTH_SIGNATURE                    (LENGTH_RS_VECTOR + MAXLENGTH_SIGN_ENCODE)


// Members to use library in blocking mode
static volatile uint8_t   m_ifx_i2c_busy = 0;
static volatile uint8_t   m_ifx_i2c_status;
static volatile uint8_t* m_optiga_rx_buffer;
static volatile uint16_t  m_optiga_rx_len;

//Preinstantiated object
AES aes = AES();
IFX_OPTIGA_TrustX trustX = IFX_OPTIGA_TrustX();
optiga_comms_t optiga_comms = {static_cast<void*>(&ifx_i2c_context_0), NULL, NULL, 0};
static host_lib_status_t optiga_comms_status;

IFX_OPTIGA_TrustX::IFX_OPTIGA_TrustX(){ active = false;}

IFX_OPTIGA_TrustX::~IFX_OPTIGA_TrustX(){}

/*
 * Local Functions
 */


/*
 * Global Functions
 */

int32_t IFX_OPTIGA_TrustX::begin(void)
{
    return begin(Wire);
}


int32_t IFX_OPTIGA_TrustX::checkChip(void)
{
	int32_t err = CMD_LIB_ERROR;
	uint8_t p_rnd[32];
	uint16_t rlen = 32;
	uint8_t p_cert[512];
	uint16_t clen = 0;
	uint8_t p_pubkey[68];
	uint8_t p_sign[70];
	uint8_t p_unformSign[66];
	uint16_t slen = 0;

	do {
		randomSeed(analogRead(0));

		for (uint8_t i = 0; i < rlen; i++) {
			p_rnd[i] = random(0xff);
			randomSeed(analogRead(0));
		}

		err = getCertificate(p_cert, clen);

		if (err)
			break;

		getPublicKey(p_pubkey);

		Serial.println("Calling calculate Signature:");
		err = calculateSignature(p_rnd, rlen, p_sign, slen);
		DEBUG_PRINT(p_sign, slen);
		Serial.println(slen, DEC);

		if (err)
			break;

		Serial.println("Processing Signature");

		if (p_sign[1] == 0x21)
		{
		  memcpy(p_unformSign, &p_sign[3], LENGTH_RS_VECTOR/2);
		  if (p_sign[(LENGTH_RS_VECTOR/2) + 4] == 0x21) {
			memcpy(&p_unformSign[LENGTH_RS_VECTOR/2], &p_sign[(LENGTH_RS_VECTOR/2) + 6], LENGTH_RS_VECTOR/2);
		  } else {
			memcpy(&p_unformSign[LENGTH_RS_VECTOR/2], &p_sign[(LENGTH_RS_VECTOR/2) + 5], LENGTH_RS_VECTOR/2);
		  }
		}
		else
		{
		  memcpy(p_unformSign, &p_sign[2], LENGTH_RS_VECTOR/2);
		  if (p_sign[(LENGTH_RS_VECTOR/2) + 3] == 0x21)
		  {
			memcpy(&p_unformSign[LENGTH_RS_VECTOR/2], &p_sign[(LENGTH_RS_VECTOR/2) + 5], (LENGTH_RS_VECTOR/2));
		  }
		  else
		  {
			memcpy(&p_unformSign[LENGTH_RS_VECTOR/2], &p_sign[(LENGTH_RS_VECTOR/2) + 4], LENGTH_RS_VECTOR/2);
		  }
		}

		Serial.println("Calling uECC_verify");
		//Serial.println("Trust X Public Key:");
		//DEBUG_PRINT(p_pubkey, 65);
		//Serial.println("Random Number:");
		//__hexdump__(p_rnd, 32);
		Serial.println("Signature:");
		//DEBUG_PRINT(p_unformSign, 65);


		if (uECC_verify(p_pubkey+4, p_rnd, rlen, p_unformSign, uECC_secp256r1())) {
			err = 0;
			Serial.println("uECC_verify Ok");

		}
		else
		{
			Serial.println("uECC_verify failed");
		}
		Serial.println("uECC_verify completed");
	} while(0);

	return err;
}

static void optiga_comms_event_handler(void* upper_layer_ctx, host_lib_status_t event)
{
    optiga_comms_status = event;
}

int32_t IFX_OPTIGA_TrustX::begin(TwoWire& CustomWire)
{

    int32_t ret = CMD_LIB_ERROR;

    sOpenApp_d openapp_opt;

    do {
        //Invoke optiga_comms_open to initialize the IFX I2C Protocol and security chip
        optiga_comms_status = OPTIGA_COMMS_BUSY;
        optiga_comms.upper_layer_handler = optiga_comms_event_handler;

        //Serial.println("calling optiga_comms_open()");

        if(E_COMMS_SUCCESS != optiga_comms_open(&optiga_comms))
        {
        	Serial.println("Error: optiga_comms_open() failed.");
            break;
        }

        //Wait until IFX I2C initialization is complete
        while(optiga_comms_status == OPTIGA_COMMS_BUSY) {
            // Push forward timer dependent actions.
            pal_os_event_process();
        }
#if 0
        if(E_COMMS_SUCCESS != optiga_comms_set_address(&optiga_comms, 0x30))
        {
        	Serial.println("Error: optiga_comms_set_address() failed.");
            break;
        }
#endif
        //Set OPTIGA comms context in Command library before invoking the use case APIs or command library APIs
        //This context will be used by command library to communicate with OPTIGA using IFX I2C Protocol.
        CmdLib_SetOptigaCommsContext(&optiga_comms);

        openapp_opt.eOpenType = eInit;

        //Serial.println("Open Trust X application");

        //Open the application in security chip
        ret = CmdLib_OpenApplication(&openapp_opt);
        if (CMD_LIB_OK == ret) {
            CmdLib_GetMaxCommsBufferSize();
            ret = 0;
			active = true;
        }else {
			Serial.print(ret, HEX);
		}

    } while (0);

    return ret;
}

//set I2C address
int32_t IFX_OPTIGA_TrustX::set_i2c_address(uint8_t address)
{
	int32_t ret = CMD_LIB_ERROR;

	//Serial.println(">IFX_OPTIGA_TrustX::set_i2c_address");

    if(E_COMMS_SUCCESS != optiga_comms_set_address(&optiga_comms, address))
    {
    	Serial.println("Error: optiga_comms_set_address() failed.");
    	return ret;
    }
    ret = 0;
    //Serial.println("<IFX_OPTIGA_TrustX::set_i2c_address");
    return ret;
}

//Restore the default I2C address
int32_t IFX_OPTIGA_TrustX::restore(void)
{
	int32_t ret = CMD_LIB_ERROR;

	//Serial.println(">IFX_OPTIGA_TrustX::restore");
    if(E_COMMS_SUCCESS != optiga_comms_set_address(&optiga_comms, 0x30))
    {
    	Serial.println("Error: optiga_comms_set_address() failed.");
    	return ret;
    }

    ret = 0;
    //Serial.println("<IFX_OPTIGA_TrustX::restore");
    return ret;
}

int32_t IFX_OPTIGA_TrustX::reset(void)
{
    // Soft reset
    optiga_comms_reset(&optiga_comms, 1);
    end();
    return begin(Wire);
}

void IFX_OPTIGA_TrustX::end(void)
{
    optiga_comms_close(&optiga_comms);
}



int32_t IFX_OPTIGA_TrustX::getGenericData(uint16_t oid, uint8_t* p_data, uint16_t& hashLength)
{
    int32_t ret = (int32_t)INT_LIB_ERROR;
    sReadGPData_d   data_opt;
    sbBlob_d        blob;

    do
    {
        if ((p_data == NULL) || (active == false)) {
            break;
        }

        //Read complete data structure
        data_opt.wOffset = 0x00;
        data_opt.wLength = hashLength;
        data_opt.wOID = oid;

        //Reading available data
        blob.prgbStream = p_data;
        blob.wLen = hashLength;
        if(INT_LIB_OK == IntLib_ReadGPData(&data_opt,&blob))
        {
            ret = 0;
            hashLength = blob.wLen;
            break;
        }

    }while(FALSE);

    return ret;
}

int32_t IFX_OPTIGA_TrustX::getState(uint16_t oid, uint8_t& byte)
{
    uint16_t length = 1;
    int32_t  ret = (int32_t)CMD_LIB_ERROR;
	uint8_t  bt = 0;
	sGetData_d sGDVector;
    sCmdResponse_d sCmdResponse;

	sGDVector.wOID = oid;
	sGDVector.wLength = 1;
	sGDVector.wOffset = 0;
	sGDVector.eDataOrMdata = eDATA;

	sCmdResponse.prgbBuffer = &bt;
	sCmdResponse.wBufferLength = 1;
	sCmdResponse.wRespLength = 0;

	ret = CmdLib_GetDataObject(&sGDVector,&sCmdResponse);
	if(CMD_LIB_OK == ret)
	{
		byte = bt;
		ret = 0;
	}

    return ret;
}

int32_t IFX_OPTIGA_TrustX::setGenericData(uint16_t oid, uint8_t* p_data, uint16_t hashLength)
{
    int32_t ret = (int32_t)CMD_LIB_ERROR;
    sSetData_d setdata_opt;

    //Set Auth scheme
    //If access condition satisfied, set the data
    setdata_opt.wOID = oid;
    setdata_opt.wOffset = 0x0000;
    setdata_opt.eDataOrMdata = eDATA;
    setdata_opt.eWriteOption = eERASE_AND_WRITE;
    setdata_opt.prgbData = p_data;
    setdata_opt.wLength = hashLength;

    ret = CmdLib_SetDataObject(&setdata_opt);

    if(CMD_LIB_OK == ret)
    {
        ret = 0;
    }
    return ret;
}
/*************************************************************************************
 *                              COMMANDS API TRUST E COMPATIBLE
 **************************************************************************************/
char * IFX_OPTIGA_TrustX::version(void)
{
    return VERSION_HOST_LIBRARY;
}

int32_t IFX_OPTIGA_TrustX::getCertificate(uint8_t* p_cert, uint16_t& clen)
{
    int32_t ret  = CMD_LIB_ERROR;
    sReadGPData_d data_opt;
    sbBlob_d cert_blob;
    uint16_t tag_len;
    uint32_t cert_len = 0;
    do
    {
#define LENGTH_CERTLIST_LEN     3
#define LENGTH_CERTLEN          3
#define LENGTH_TAGlEN_PLUS_TAG  3
#define LENGTH_MINIMUM_DATA     10

        if ((p_cert == NULL)  || (active == false)) {

            break;
        }
        //Read complete certificate
        data_opt.wOffset = 0x00;
        data_opt.wLength = 0xFFFF;
        data_opt.wOID = OID_IFX_CERTIFICATE;

        //Reading available certificate data
        cert_blob.prgbStream = p_cert;
        cert_blob.wLen = LENGTH_CERTIFICATE;
        ret = IntLib_ReadGPData(&data_opt,&cert_blob);
        if(INT_LIB_OK != ret)
        {
            break;
        }

        //Validate TLV
        if((TLS_TAG != p_cert[0]) && (ASN_TAG_SEQUENCE != p_cert[0]))
        {
            break;
        }

        if(TLS_TAG == p_cert[0])
        {
            //Check minimum length must be 10
            if(cert_blob.wLen < LENGTH_MINIMUM_DATA)
            {
                break;
            }
            tag_len = Utility_GetUint16 (&p_cert[1]);
            cert_len = Utility_GetUint24(&p_cert[6]);
            //Length checks
            if((tag_len != (cert_blob.wLen - LENGTH_TAGlEN_PLUS_TAG)) ||           \
                (Utility_GetUint24(&p_cert[3]) != (uint32_t)(tag_len - LENGTH_CERTLIST_LEN)) ||   \
                ((cert_len > (uint32_t)(tag_len - (LENGTH_CERTLIST_LEN  + LENGTH_CERTLEN))) || (cert_len == 0x00)))
            {
                break;
            }
        }

        memmove(&p_cert[0], &p_cert[9], cert_len);
        clen = (uint16_t)cert_len;
        ret = 0;
    } while (FALSE);

    return ret;
}

int32_t IFX_OPTIGA_TrustX::getPublicKey(uint8_t p_pubkey[64])
{
	int32_t ret = CMD_LIB_ERROR;
	uint8_t p_cert[512];
	uint16_t clen = 0;

	do{
		ret = getCertificate(p_cert, clen);
		if (ret)
			break;

		if ((p_cert != NULL) || (p_pubkey != NULL)) {
			  for (uint16_t i=0; i < clen; i++) {
				if (p_cert[i] != 0x03)
				  continue;
				if (p_cert[i+1] != 0x42)
				  continue;
				if (p_cert[i+2] != 0x00)
				  continue;
				if (p_cert[i+3] != 0x04)
				  continue;

				memcpy(p_pubkey, &p_cert[i], 68);
			  }
		}

		ret = 0;
	} while (FALSE);

	return ret;
}

int32_t IFX_OPTIGA_TrustX::getRandom(uint16_t length, uint8_t* p_random)
{
    int32_t ret = (int32_t)CMD_LIB_ERROR;
    sRngOptions_d rng_opt;
    sCmdResponse_d cmd_resp;

    rng_opt.eRngType = eTRNG;
    rng_opt.wRandomDataLen = length;

    cmd_resp.prgbBuffer = p_random;
    cmd_resp.wBufferLength = length;

    do {
        if (cmd_resp.prgbBuffer == NULL || (active == false)) {
            ret = 1;
            break;
        }

        ret = CmdLib_GetRandom(&rng_opt, &cmd_resp);
        if(CMD_LIB_OK == ret)
        {
            ret = 0;
        }

    }while(0);

    return ret;
}


int32_t IFX_OPTIGA_TrustX::sha256(uint8_t dataToHash[], uint16_t ilen, uint8_t out[32])
{
    uint16_t ret = 1;

    sCalcHash_d calchash_opt;

    do {
        calchash_opt.eHashAlg = eSHA256;
        calchash_opt.eHashSequence  = eStartFinalizeHash;
        calchash_opt.eHashDataType = eDataStream;
        calchash_opt.sDataStream.prgbStream = dataToHash;
        calchash_opt.sDataStream.wLen = ilen;
        calchash_opt.sContextInfo.dwContextLen = 0x00;
        calchash_opt.sContextInfo.pbContextData = NULL;
        calchash_opt.sContextInfo.eContextAction = eUnused;
        calchash_opt.sOutHash.prgbBuffer = out;
        calchash_opt.sOutHash.wBufferLength = 32;
        calchash_opt.sOutHash.wRespLength = 0;

        if (CMD_LIB_OK == CmdLib_CalcHash(&calchash_opt))
        {
            ret = 0;
            break;
        }

//      //eContinueHash - OID
//      calchash_opt.eHashSequence  = eContinueHash;
//      calchash_opt.eHashDataType = eOIDData;
//      calchash_opt.sOIDData.wOID = (uint16_t)eDEVICE_PUBKEY_CERT_IFX;
//      calchash_opt.sOIDData.wOffset = 0x00;
//      calchash_opt.sOIDData.wLength = 0x0020;
//      //In case of Intermediate Hash
//      //Set the variables as shown below
//      //calchash_opt.eHashSequence = eIntermediateHash;
//      //Allocate the buffer to stor ethe Hash output
//      //calchash_opt.sOutHash.prgbBuffer = rgbOutBuffer;
//      //calchash_opt.sOutHash.wBufferLength = sizeof(rgbOutBuffer);
//      //calchash_opt.sOutHash.wRespLength = 0;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //efinalizeHash - Datastream
//      calchash_opt.eHashSequence  = eFinalizeHash;
//      calchash_opt.eHashDataType = eDataStream;
//      calchash_opt.sDataStream.prgbStream = rgbDataStream;
//      calchash_opt.sDataStream.wLen = sizeof(rgbDataStream);
//
//      //Allocate output buffer for eFinalize
//      calchash_opt.sOutHash.prgbBuffer = rgbOutBuffer;
//      calchash_opt.sOutHash.wBufferLength = sizeof(rgbOutBuffer);
//      calchash_opt.sOutHash.wRespLength = 0;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //
//      //Import and Export of Hash Context
//      //
//
//      //eStart
//      calchash_opt.eHashAlg = eSHA256;
//      calchash_opt.eHashSequence  = eStartHash;
//      calchash_opt.eHashDataType = eDataStream;
//      calchash_opt.sDataStream.prgbStream = rgbDataStream;
//      calchash_opt.sDataStream.wLen = 10;
//      calchash_opt.sContextInfo.eContextAction = eUnused;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //First eContinue and eExport
//      calchash_opt.eHashAlg = eSHA256;
//      calchash_opt.eHashSequence  = eContinueHash;
//      calchash_opt.eHashDataType = eDataStream;
//      calchash_opt.sDataStream.prgbStream = &rgbDataStream[10];
//      calchash_opt.sDataStream.wLen = 10;
//      calchash_opt.sContextInfo.dwContextLen = sizeof(rgbFirstHashCntx);
//      calchash_opt.sContextInfo.pbContextData = rgbFirstHashCntx;
//      calchash_opt.sContextInfo.eContextAction = eExport;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //eStart
//      calchash_opt.eHashAlg = eSHA256;
//      calchash_opt.eHashSequence  = eStartHash;
//      calchash_opt.eHashDataType = eOIDData;
//      calchash_opt.eHashDataType = eOIDData;
//      calchash_opt.sOIDData.wOID = (uint16_t)eDEVICE_PUBKEY_CERT_IFX;
//      calchash_opt.sOIDData.wOffset = 0x00;
//      calchash_opt.sOIDData.wLength = 0x0020;
//      calchash_opt.sContextInfo.eContextAction = eUnused;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //Second eContinue and eExport
//      calchash_opt.eHashAlg = eSHA256;
//      calchash_opt.eHashSequence  = eContinueHash;
//      calchash_opt.eHashDataType = eDataStream;
//      calchash_opt.sDataStream.prgbStream = &rgbDataStream[20];
//      calchash_opt.sDataStream.wLen = 10;
//      calchash_opt.sContextInfo.dwContextLen = sizeof(rgbSecondHashCntx);
//      calchash_opt.sContextInfo.pbContextData = rgbSecondHashCntx;
//      calchash_opt.sContextInfo.eContextAction = eExport;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //eContinue and eImport of First HashCntx
//      calchash_opt.eHashAlg = eSHA256;
//      calchash_opt.eHashSequence  = eContinueHash;
//      calchash_opt.eHashDataType = eDataStream;
//      calchash_opt.sDataStream.prgbStream = &rgbDataStream[20];
//      calchash_opt.sDataStream.wLen = 10;
//      calchash_opt.sContextInfo.dwContextLen = sizeof(rgbFirstHashCntx);
//      calchash_opt.sContextInfo.pbContextData = rgbFirstHashCntx;
//      calchash_opt.sContextInfo.eContextAction = eImport;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //efinalizeHash with First Hash Context
//      calchash_opt.eHashSequence  = eFinalizeHash;
//      calchash_opt.eHashDataType = eDataStream;
//      calchash_opt.sDataStream.prgbStream = rgbDataStream;
//      calchash_opt.sDataStream.wLen = sizeof(rgbDataStream);
//
//      //Allocate output buffer for eFinalize
//      calchash_opt.sOutHash.prgbBuffer = rgbOutBuffer;
//      calchash_opt.sOutHash.wBufferLength = sizeof(rgbOutBuffer);
//      calchash_opt.sOutHash.wRespLength = 0;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //eContinue and eImport of Second HashCntx
//      calchash_opt.eHashAlg = eSHA256;
//      calchash_opt.eHashSequence  = eContinueHash;
//      calchash_opt.eHashDataType = eDataStream;
//      calchash_opt.sDataStream.prgbStream = &rgbDataStream[20];
//      calchash_opt.sDataStream.wLen = 10;
//      calchash_opt.sContextInfo.dwContextLen = sizeof(rgbSecondHashCntx);
//      calchash_opt.sContextInfo.pbContextData = rgbSecondHashCntx;
//      calchash_opt.sContextInfo.eContextAction = eImport;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }
//
//      //efinalizeHash with Second Hash Context
//      calchash_opt.eHashSequence  = eFinalizeHash;
//      calchash_opt.eHashDataType = eDataStream;
//      calchash_opt.sDataStream.prgbStream = rgbDataStream;
//      calchash_opt.sDataStream.wLen = sizeof(rgbDataStream);
//
//      //Allocate output buffer for eFinalize
//      calchash_opt.sOutHash.prgbBuffer = rgbOutBuffer;
//      calchash_opt.sOutHash.wBufferLength = sizeof(rgbOutBuffer);
//      calchash_opt.sOutHash.wRespLength = 0;
//      ret = CmdLib_CalcHash(&calchash_opt);
//      if(CMD_LIB_OK != ret)
//      {
//          break;
//      }

    }while(FALSE);

    return ret;
}

int32_t IFX_OPTIGA_TrustX::calculateSignature(uint8_t dataToSign[], uint16_t ilen, uint16_t ctx, uint8_t* out, uint16_t& olen)
{
    uint16_t ret = (int32_t)INT_LIB_ERROR;
    sCalcSignOptions_d calsign_opt;
    sbBlob_d sign_blob;
#define MAX_SIGN_LEN    80

    do
    {
        if (dataToSign == NULL || out == NULL)
        {
            break;
        }

        //
        // Example to demonstrate the calc sign using the private key object
        //
        calsign_opt.eSignScheme = eECDSA_FIPS_186_3_WITHOUT_HASH;
        calsign_opt.sDigestToSign.prgbStream = dataToSign;
        calsign_opt.sDigestToSign.wLen = ilen;

        //Choose the key OID from the device private keys or session private keys.
        //Note: Make sure the private key is available in the OID
        calsign_opt.wOIDSignKey = (uint16_t)ctx;

        sign_blob.prgbStream = out;
        sign_blob.wLen = MAX_SIGN_LEN;

        //Initiate CmdLib API for the Calculation of signature
        if(CMD_LIB_OK == CmdLib_CalculateSign(&calsign_opt,&sign_blob))
        {
            olen = sign_blob.wLen;
            ret = 0;
            //Print_Stringline("Calculation of Signature is successful");
        }
    }while(FALSE);

    return ret;
}

int32_t IFX_OPTIGA_TrustX::formatSignature(uint8_t* inSign, uint16_t signLen, uint8_t* outSign, uint16_t& outSignLen)
{
    int32_t ret = (int32_t)INT_LIB_ERROR;
    uint8_t bIndex = 0;
    do
    {
        if((NULL == outSign) || (NULL == inSign))
        {
            ret = (int32_t)INT_LIB_NULL_PARAM;
            break;
        }
        if((0 == outSignLen)||(0 == signLen))
        {
            ret = (int32_t)INT_LIB_ZEROLEN_ERROR;
            break;
        }
        //check to see oif input buffer is short,
        // or signture plus 6 byte considering der encoding  is more than 0xff
        if((outSignLen < signLen)||(0xFF < (signLen + 6)))
        {
            //send lib error
            break;
        }
        //Encode ASN sequence
        *(outSign + 0) = ASN_TAG_SEQUENCE;
        //Length of RS and encoding bytes
        *(outSign + 1) = LENGTH_RS_VECTOR + 4;
        //Encode integer
        *(outSign + 2) = ASN_TAG_INTEGER;
        //Check if the integer is negative
        bIndex = 4;
        *(outSign + 3) = 0x20;
        if(outSign[0] & MASK_MSB)
        {
            *(outSign + 3) = 0x21;
            *(outSign + bIndex++) = 0x00;
        }

        //copy R
        memmove(outSign + bIndex, inSign, (LENGTH_RS_VECTOR/2));
        bIndex+=(LENGTH_RS_VECTOR/2);
        //Encode integer
        *(outSign + bIndex++) = ASN_TAG_INTEGER;
        //Check if the integer is negative
        *(outSign + bIndex) = 0x20;
        if(outSign[LENGTH_RS_VECTOR/2] & MASK_MSB)
        {
            *(outSign + bIndex) = 0x21;
            bIndex++;
            *(outSign + bIndex) = 0x00;
        }
        bIndex++;

        //copy S
        memcpy(outSign + bIndex, inSign+(LENGTH_RS_VECTOR/2), (LENGTH_RS_VECTOR/2));
        bIndex += (LENGTH_RS_VECTOR/2);
        //Sequence length is "index-2"
        *(outSign + 1) = (bIndex-2);
        //Total length is equal to index
        outSignLen = bIndex;

        ret = 0;

    }while(FALSE);

    return ret;
}

int32_t IFX_OPTIGA_TrustX::verifySignature( uint8_t* digest, uint16_t hashLength,
                                         uint8_t* sign, uint16_t signatureLength,
                                         uint8_t* pubKey, uint16_t plen)
{
    int32_t ret = (int32_t)INT_LIB_ERROR;
    sVerifyOption_d versign_opt;
    sbBlob_d sign_blob, digest_blob;

    //
    // Example to demonstrate the verifySignature using the Public Key from Host
    //
    versign_opt.eSignScheme = eECDSA_FIPS_186_3_WITHOUT_HASH;
    versign_opt.eVerifyDataType = eDataStream;
    versign_opt.sPubKeyInput.eAlgId = eECC_NIST_P256;
    versign_opt.sPubKeyInput.sDataStream.prgbStream = pubKey;
    versign_opt.sPubKeyInput.sDataStream.wLen = plen;

    digest_blob.prgbStream = digest;
    digest_blob.wLen = hashLength;

    sign_blob.prgbStream = sign;
    sign_blob.wLen = signatureLength;

    //Initiate CmdLib API for the Verification of signature
    ret = CmdLib_VerifySign(&versign_opt, &digest_blob, &sign_blob);

    if(CMD_LIB_OK == ret)
    {
        ret = 0;
    }

    return ret;
}

int32_t IFX_OPTIGA_TrustX::verifySignature( uint8_t* digest, uint16_t hashLength,
											uint8_t* sign, uint16_t signatureLength,
											uint16_t publicKey_oid )
{
    int32_t ret = (int32_t)INT_LIB_ERROR;
    sVerifyOption_d versign_opt;
    sbBlob_d sign_blob;
    sbBlob_d digest_blob;

    do
    {
        //
        // Example to demonstrate the verifySignature using the Public Key from Host
        //
        versign_opt.eSignScheme = eECDSA_FIPS_186_3_WITHOUT_HASH;
        versign_opt.eVerifyDataType = eOIDData;
        versign_opt.wOIDPubKey = publicKey_oid;

        digest_blob.prgbStream = digest;
        digest_blob.wLen = hashLength;

        sign_blob.prgbStream = sign;
        sign_blob.wLen = signatureLength;

        //Initiate CmdLib API for the Verification of signature
        ret = CmdLib_VerifySign(&versign_opt, &digest_blob, &sign_blob);

        if(CMD_LIB_OK == ret)
        {
            ret = 0;
        }
    }while(FALSE);

    return ret;
}

int32_t IFX_OPTIGA_TrustX::calculateSharedSecretGeneric(int32_t curveID,
		                                                uint16_t PrivateKey_OID,
														uint8_t* PublicKey,
														uint16_t PublicKey_Len,
														uint16_t SharedSecret_OID,
														uint8_t* ExportShareSecret,
														uint16_t& ExportShareSecret_Len)
{
    int32_t             ret = IFX_I2C_STACK_ERROR;
    sCalcSSecOptions_d  shsec_opt;
    sbBlob_d            shsec;

    uint8_t             ShareSecret[ExportShareSecret_Len];

    //Serial.println(">calculateSharedSecretGeneric");

    //Mention the Key Agreement protocol
    shsec_opt.eKeyAgreementType = eECDH_NISTSP80056A;

    //Provide the public key information
    shsec_opt.ePubKeyAlgId          = (eAlgId_d)curveID;
    shsec_opt.sPubKey.prgbStream    = PublicKey;
    shsec_opt.sPubKey.wLen          = PublicKey_Len;

    //Provide the ID of the private key to be used
    //Make sure the private key is present in the OID. Use CmdLib_GenerateKeyPair
    shsec_opt.wOIDPrivKey = PrivateKey_OID;

    //Mentioned where should the generated shared secret be stored.
    //1.To store the shared secret in session oid,provide the session oid value
    //or
    //2.To export the shared secret, set the value to 0x0000
    shsec_opt.wOIDSharedSecret = SharedSecret_OID;

    //Buffer to export the generated shared secret
    //Shared secret is returned if sCalcSSecOptions.wOIDSharedSecret is 0x0000.
    shsec.prgbStream = ShareSecret;
    shsec.wLen = sizeof(ShareSecret);

    //Initiate CmdLib API for the Calculate shared secret
    ret = CmdLib_CalculateSharedSecret(&shsec_opt, &shsec);
    if(CMD_LIB_OK == ret)
    {
    	//Serial.println("calculateSharedSecretGeneric:Ok");
		if(SharedSecret_OID==0x0000)
		{
			memcpy(ExportShareSecret, ShareSecret, ExportShareSecret_Len);

			//Serial.println("Export Share Secret in Plaintext:");
			//DEBUG_PRINT(ExportShareSecret, ExportShareSecret_Len);

		}
		else
		{
			//Serial.print("Share Secret stored in OID: 0x");
			//Serial.println(SharedSecret_OID,HEX);

		}
		ExportShareSecret_Len = shsec.wLen;
        ret = 0;
    }else
    {
    	Serial.println("calculateSharedSecretGeneric:Error");
    	Serial.println(ret,HEX);

    }

    //Serial.println("<calculateSharedSecretGeneric");
    return ret;
}

int32_t IFX_OPTIGA_TrustX::str2cur(String curve_name)
{
    int32_t ret;

    if (curve_name == "secp256r1") {
        ret = eECC_NIST_P256;
    } else if (curve_name == "secp384r1") {
        ret = eECC_NIST_P384;
    } else {
        ret = eECC_NIST_P256;
    }

    return ret;
}

int32_t IFX_OPTIGA_TrustX::deriveKey(uint16_t ShareSecret_OID,
		                             uint16_t ShareSecret_OID_Len,
									 uint16_t DeriveKey_OID,
									 int8_t* ExportDeriveKey,
									 int8_t ExportDeriveKey_Len
									 )
{
    int32_t             ret = INT_LIB_ERROR;
    sDeriveKeyOptions_d key_opt;
    sbBlob_d            key;
    //For now, hard code the seed and length of derive key
    uint8_t rgbSeed [] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};

    uint8_t DeriveKey[256];

    //
    // Example to demonstrate the derive key
    //

    //Serial.println(">deriveKey");

    //Mention the Key derivation method
    key_opt.eKDM = eTLS_PRF_SHA256;

    //Provide the seed information (min len 8 bytes, Max 1024 bytes)
    key_opt.sSeed.prgbStream = rgbSeed;
    key_opt.sSeed.wLen =  sizeof(rgbSeed);

    //Provide the ID of the share secret to be used
    //Make sure the shared secret is present in the OID. Use CmdLib_CalculateSharedSecret
    // OID Master Secret
    key_opt.wOIDSharedSecret = ShareSecret_OID;

	//Serial.print("Share Secret stored in OID: 0x");
	//Serial.println(ShareSecret_OID,HEX);


	key_opt.wOIDDerivedKey = DeriveKey_OID;
    key_opt.wDerivedKeyLen = ShareSecret_OID_Len; //default

    //Buffer to export the generated derive key
    //Shared secret is returned if sDeriveKeyOptions.wOIDDerivedKey is 0x0000.
    key.prgbStream = DeriveKey;

    //Provide the expected length of the derive secret
	//min length is 16, max is 256 bytes
	if(ExportDeriveKey_Len < 8 || ExportDeriveKey_Len > 256)
    {
		Serial.println("Error: Invalid length using default 16 bytes");
		key.wLen = 16;
    }
	else
	{
		key_opt.wDerivedKeyLen =ExportDeriveKey_Len;
		key.wLen =ExportDeriveKey_Len;
	}

    //Initiate CmdLib API for the Calculate shared secret
    if(CMD_LIB_OK == CmdLib_DeriveKey(&key_opt, &key))
    {

    	//Serial.println("deriveKey:Ok");

    	if(DeriveKey_OID==0x0000)
    	{
			memcpy(ExportDeriveKey, DeriveKey, ExportDeriveKey_Len);

			//Serial.println("Exporting derive Secret:");
			//DEBUG_PRINT(ExportDeriveKey, ExportDeriveKey_Len);
    	}else
    	{

    		//Serial.println("Derive Secret stored: 0x");
    		//Serial.println(DeriveKey_OID,HEX);

    	}

        //klen = key.wLen;
        ret = 0;
    }


    //Serial.println("<deriveKey");

    return ret;
}

int32_t IFX_OPTIGA_TrustX::generateKeypair(uint8_t* p_pubkey, uint16_t& plen, uint16_t privkey_oid)
{
    int32_t ret = (int32_t)INT_LIB_ERROR;
    sKeyPairOption_d keypair_opt;
    sOutKeyPair_d    keypair;

    do
    {
        if (p_pubkey == NULL) {
            break;
        }
        // Example to demonstrate the Generate KeyPair and use the private and public keys.
        // The keys generated can be used for calculation and verification of signature using-
        // the toolbox command examples specified below.

        keypair_opt.eAlgId = eECC_NIST_P256;
        keypair_opt.eKeyExport = eStorePrivKeyOnly;
        if (privkey_oid == 0)
        {
            keypair_opt.wOIDPrivKey= (uint16_t)eSESSION_ID_2;
          }
        else
        {

          if((privkey_oid == eSESSION_ID_1) ||
             (privkey_oid == eSESSION_ID_2) ||
             (privkey_oid == eSESSION_ID_3) ||
             (privkey_oid == eSESSION_ID_4) ||
             (privkey_oid == eFIRST_DEVICE_PRIKEY_2) ||
             (privkey_oid == eFIRST_DEVICE_PRIKEY_3) ||
             (privkey_oid == eFIRST_DEVICE_PRIKEY_4))
          {
            keypair_opt.wOIDPrivKey= (uint16_t)privkey_oid;
          }
          else
          {
              return ret;
          }
        }

        // Select the key usage identifier for authentication, signing and key agreement (shared secret) use cases.
        keypair_opt.eKeyUsage = (eKeyUsage_d)(eKeyAgreement | eAuthentication | eSign);

        keypair.sPublicKey.prgbStream = p_pubkey;
        keypair.sPublicKey.wLen = 80;

        //Initiate CmdLib API for the generate the key pair. The private key gets stored in the-
        // session context OID 0xE101 and public key is exported out.
        ret = CmdLib_GenerateKeyPair(&keypair_opt,&keypair);

        if(CMD_LIB_OK == ret)
        {
            plen = keypair.sPublicKey.wLen;
            ret = 0;
            break;
        }

    }while(FALSE);

    return ret;
}

int32_t IFX_OPTIGA_TrustX::generateKeypair(uint8_t* p_pubkey, uint16_t& plen, uint8_t* p_privkey, uint16_t& prlen)
{
    int32_t ret = (int32_t)INT_LIB_ERROR;
    sKeyPairOption_d keypair_opt;
    sOutKeyPair_d    keypair;

    do
    {
        if (p_pubkey == NULL || p_privkey == NULL) {
            break;
        }
        // Example to demonstrate the Generate KeyPair and use the private and public keys.
        // The keys generated can be used for calculation and verification of signature using-
        // the toolbox command examples specified below.

        keypair_opt.eAlgId = eECC_NIST_P256;
        keypair_opt.eKeyExport = eExportKeyPair;
        keypair_opt.wOIDPrivKey= (uint16_t)eSESSION_ID_2;

        // Select the key usage identifier for authentication, signing and key agreement (shared secret) use cases.
        keypair_opt.eKeyUsage = (eKeyUsage_d)(eKeyAgreement | eAuthentication | eSign);

        keypair.sPublicKey.prgbStream = p_pubkey;
        keypair.sPublicKey.wLen = 80;
        keypair.sPrivateKey.prgbStream = p_privkey;
        keypair.sPrivateKey.wLen = 80;

        //Initiate CmdLib API for the generate the key pair. The private key gets stored in the-
        // session context OID 0xE101 and public key is exported out.
        ret = CmdLib_GenerateKeyPair(&keypair_opt,&keypair);

        if(CMD_LIB_OK == ret)
        {
            plen = keypair.sPublicKey.wLen;
            prlen = keypair.sPrivateKey.wLen;
            ret = 0;
            break;
        }

    }while(FALSE);

    return ret;
}
