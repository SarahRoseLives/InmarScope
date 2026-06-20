/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#ifndef AEROL_H
#define AEROL_H

#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <assert.h>
#include "jconvolutionalcodec.h"
#include "iostream"

namespace AEROTypeR {
typedef enum MessageType
{
    General_access_request_telephone=0x20,
    Abbreviated_access_request_telephone=0x23,
    Access_request_data_R_T_channel=0x22,
    Request_for_acknowledgement_R_channel=0x61,
    Acknowledgement_R_channel=0x62,
    Log_On_Off_control_R_channel=0x12,
    Call_progress_R_channel=0x30,
    Log_On_Off_acknowledgement=0x15,
    Log_control_R_channel_ready_for_reassignment=0x17,
    Telephony_acknowledge_R_channel=0x60,
    User_data_ISU_SSU_R_channel=-1,
} MessageType;
}

namespace AEROTypeT {
typedef enum MessageType
{
    Fill_in_signal_unit=0x01,
    User_data_ISU_RLS_T_channel=0x71,
    User_data_ISU_SSU_T_channel=-1,
} MessageType;
}

namespace AEROTypeC {
typedef enum MessageType
{
    Fill_in_signal_unit=0x01,
    Call_progress=0x30,
    Telephony_acknowledge=0x60,
} MessageType;
}

namespace AEROTypeP {
typedef enum MessageType
{
    Reserved_0=0x00,
    Fill_in_signal_unit=0x01,

    AES_system_table_broadcast_GES_Psmc_and_Rsmc_channels_COMPLETE=0x05,
    AES_system_table_broadcast_GES_beam_support_COMPLETE=0x07,
    AES_system_table_broadcast_index=0x0A,
    AES_system_table_broadcast_satellite_identification_COMPLETE=0x0C,

    //SYSTEM LOG-ON/LOG-OFF
    Log_on_request=0x10,
    Log_on_confirm=0x11,
    Log_control_P_channel_log_off_request=0x12,
    Log_control_P_channel_log_on_reject=0x13,
    Log_control_P_channel_log_on_interrogation=0x14,
    Log_on_log_off_acknowledge_P_channel=0x15,
    Log_control_P_channel_log_on_prompt=0x16,
    Log_control_P_channel_data_channel_reassignment=0x17,

    Reserved_18=0x18,
    Reserved_19=0x19,
    Reserved_26=0x26,

    //CALL INITIATION
    Call_announcement=0x21,

    Data_EIRP_table_broadcast_complete_sequence=0x28,

    // C CHANNEL RELATED
    Call_progress=0x30,
    C_channel_assignment_distress=0x31,
    C_channel_assignment_flight_safety=0x32,
    C_channel_assignment_other_safety=0x33,
    C_channel_assignment_non_safety=0x34,

    //CHANNEL INFORMATION
    P_R_channel_control_ISU=0x40,
    T_channel_control_ISU=0x41,

    T_channel_assignment=0x51,




    //ACKNOWLEDGEMENT
    Request_for_acknowledgement_RQA_P_channel=0x61,
    Acknowledge_RACK_TACK_P_channel=0x62,


    User_data_ISU_RLS_P_T_channel=0x71,
    User_data_3_octet_LSDU_RLS_P_channel=0x74,
    User_data_4_octet_LSDU_RLS_P_channel=0x76


} MessageType;

}

struct ISUItem {
    uint32_t AESID;
    uint8_t GESID;
    uint8_t QNO;
    uint8_t SEQNO;
    uint8_t REFNO;
    uint8_t NOOCTLESTINLASTSSU;
    std::vector<uint8_t> userdata;
    int count;
    void clear()
    {
        AESID=0;
        GESID=0;
        QNO=0;
        SEQNO=0;
        REFNO=0;
        NOOCTLESTINLASTSSU=0;
        userdata.clear();
        count=0;
    }
};

struct RISUItem : ISUItem{
    int SEQINDICATOR;
    int SUTYPE;
    int filledarray;
    void clear()
    {
        ISUItem::clear();
        SEQINDICATOR=0;//for R
        SUTYPE=0;//for R
        filledarray=0;//for R
    }
};

//to do add plane info
class CChannelAssignmentItem
{
public:
    uint32_t AESID;
    uint8_t GESID;
    uint8_t INIT_ERIP;
    uint8_t REFNO;
    double receive_freq;
    double transmit_freq;
    bool receive_spotbeam;
    bool transmit_spotbeam;
    uint8_t type;
    void clear()
    {
        receive_spotbeam=false;
        transmit_spotbeam=false;
        receive_freq=0;
        transmit_freq=0;
        REFNO=0;
        INIT_ERIP=0;
        GESID=0;
        AESID=0;
        type=0;
    }
    CChannelAssignmentItem(){clear();}
};

class ACARSItem
{
public:

    ISUItem isuitem;

    char MODE;
    uint8_t TAK;
    std::vector<uint8_t> LABEL;
    uint8_t BI;
    std::vector<uint8_t> PLANEREG;
    bool nonacars;

    bool downlink;
    bool valid;
    bool hastext;
    bool moretocome;
    std::vector<uint8_t> message;
    void clear()
    {
        isuitem.clear();
        valid=false;
        hastext=false;
        moretocome=false;
        MODE=0;
        TAK=0;
        BI=0;
        nonacars=false;
        PLANEREG.clear();
        LABEL.clear();
        message.clear();
        downlink=false;
    }
    explicit ACARSItem(){clear();}
};

//defragments 0x71 SUs with its SSUs
class ISUData
{
public:
    ISUData(){missingssu=false;}
    bool update(std::vector<uint8_t> data);
    ISUItem lastvalidisuitem;
    bool missingssu;
    void reset(){isuitems.clear();}
private:
    std::vector<ISUItem> isuitems;
    ISUItem anisuitem;
    int findisuitemC0(ISUItem &anisuitem);
    int findisuitem71(ISUItem &anisuitem);
    void deleteoldisuitems();
};

//defragments R channel SUs with its SSUs
class RISUData
{
public:
    RISUData(){}
    bool update(std::vector<uint8_t> data);
    RISUItem lastvalidisuitem;
    void reset(){isuitems.clear();}
private:
    std::vector<RISUItem> isuitems;
    RISUItem anisuitem;
    int findisuitem(RISUItem &anisuitem);
    void deleteoldisuitems();
};


class ACARSDefragmenter
{
public:
    ACARSDefragmenter(){}
    bool defragment(ACARSItem &acarsitem);
private:
    struct ACARSItemext{
        ACARSItem anacarsitem;
        int count;
    };
    std::vector<ACARSItemext> acarsitemexts;
    ACARSItemext anacarsitemext;
    int findfragment(ACARSItem &acarsitem);
};

class ParserISU
{
public:
    explicit ParserISU();
    bool parse(ISUItem &isuitem);
    bool downlink;

    /* callback for decoded ACARS items */
    typedef void (*acars_cb_t)(ACARSItem &acarsitem, void *user);
    void setACARSCallback(acars_cb_t cb, void *user) { acars_callback = cb; acars_user = user; }
private:
    ACARSDefragmenter acarsdefragmenter;
    ACARSItem anacarsitem;
    acars_cb_t acars_callback;
    void *acars_user;
};

class AeroLcrc16 //this seems to be called GENIBUS not CCITT
{
public:
    AeroLcrc16(){}
    bool calcusingbitsandcheck(int *bits,int numberofbits)
    {

        uint16_t crc_rec=0;
        for(int i=numberofbits-1;i>=numberofbits-16; i--)
        {
            crc_rec<<=1;
            crc_rec|=bits[i];
        }
        numberofbits-=16;

        uint16_t crc = 0xFFFF;
        int crc_bit;
        for(int i=0; i<numberofbits; i++)//we are finished when all bits of the message are looked at
        {
            //differnt endiness
            crc_bit = crc & 1;//bit of crc we are working on. 15=poly order-1
            crc >>= 1;//shift to next crc bit (have to do this here before Gate A does its thing)
            if(crc_bit ^ bits[i])crc = crc ^ 0x8408;//(0x8408 is reversed 0x1021)add to the crc the poly mod2 if crc_bit + block_bit = 1 mod2 (0x1021 is the ploy with the first bit missing so this means x^16+x^12+x^5+1)

        }
        crc=~crc;
        if(crc_rec==crc)return true;
        return false;
    }
    uint16_t calcusingbits(int *bits,int numberofbits)
    {
        uint16_t crc = 0xFFFF;
        int crc_bit;
        for(int i=0; i<numberofbits; i++)//we are finished when all bits of the message are looked at
        {
            //differnt endiness
            crc_bit = crc & 1;//bit of crc we are working on. 15=poly order-1
            crc >>= 1;//shift to next crc bit (have to do this here before Gate A does its thing)
            if(crc_bit ^ bits[i])crc = crc ^ 0x8408;//(0x8408 is reversed 0x1021)add to the crc the poly mod2 if crc_bit + block_bit = 1 mod2 (0x1021 is the ploy with the first bit missing so this means x^16+x^12+x^5+1)

        }
        return ~crc;
    }
    uint16_t calcusingbytes(char *bytes,int numberofbytes)
    {
        uint16_t crc = 0xFFFF;
        int crc_bit;
        int message_bit;
        int message_byte;
        for(int i=0; i<numberofbytes; i++)//we are finished when all bits of the message are looked at
        {
            message_byte=bytes[i];
            for(int k=0;k<8;k++)
            {
                //differnt endiness
                message_bit=message_byte&1;
                message_byte>>=1;
                crc_bit = crc & 1;//bit of crc we are working on. 15=poly order-1
                crc >>= 1;//shift to next crc bit (have to do this here before Gate A does its thing)
                if(crc_bit ^ message_bit)crc = crc ^ 0x8408;//(0x8408 is reversed 0x1021)add to the crc the poly mod2 if crc_bit + block_bit = 1 mod2 (0x1021 is the ploy with the first bit missing so this means x^16+x^12+x^5+1)

            }
        }
        return ~crc;
    }
    uint16_t calcusingbytesotherendines(char *bytes,int numberofbytes)
    {
        uint16_t crc = 0xFFFF;
        int crc_bit;
        int message_bit;
        int message_byte;
        for(int i=0; i<numberofbytes; i++)//we are finished when all bits of the message are looked at
        {
            message_byte=bytes[i];
            for(int k=0;k<8;k++)
            {

                message_bit=(message_byte>>7)&1;
                message_byte<<=1;
                crc_bit = (crc >> 15) & 1;//bit of crc we are working on. 15=poly order-1
                crc <<= 1;//shift to next crc bit (have to do this here before Gate A does its thing)
                if(crc_bit ^ message_bit)crc = crc ^ 0x1021;//add to the crc the poly mod2 if crc_bit + block_bit = 1 mod2 (0x1021 is the ploy with the first bit missing so this means x^16+x^12+x^5+1)

            }
        }
        return ~crc;
    }
};

class AeroLScrambler
{
public:
    AeroLScrambler()
    {
        std::vector<int> state;
        position=0;
        pre_state.resize(5000);
        int tmp[]={1,1,0,1,0,0,1,0,1,0,1,1,0,0,1,-1};
        state.clear();
        for(int i=0;tmp[i]>=0;i++)state.push_back(tmp[i]);


        // populate the vector so we can reuse it
        for(int a = 0; a<5000;a++)
        {

                int val0 = state[0]^state[14];
                pre_state[a]=val0;
                for(int i=(int)state.size()-1;i>0;i--)
                {
                    state[i]=state[i-1];
    }
                state[0] =val0;

        }
    }

    void update(std::vector<int> &data)
    {
        for(int j=0;j<(int)data.size();j++)
        {
            data[j] = data[j]^pre_state[position];
            position++;
        }

        }
    void reset()
    {

        position = 0;
    }
private:

    std::vector<int> pre_state;
    int position;
};

class PuncturedCode
{
public:
    PuncturedCode();
    void depunture_soft_block(std::vector<uint8_t> &block, std::vector<uint8_t> &targetblock, int pattern, bool reset=true);
    void punture_soft_block(std::vector<uint8_t> &block, int pattern, bool reset=true);
private:
    int punture_ptr;
    int depunture_ptr;
};

class DelayLine
{
public:
    DelayLine()
    {
        setLength(12);
    }
    void setLength(int length)
    {
        length++;
        assert(length>0);
        buffer.resize(length);
        buffer_ptr=0;
        buffer_sz=buffer.size();
    }
    void update(std::vector<int> &data)
    {
        for(int i=0;i<(int)data.size();i++)
        {
            buffer[buffer_ptr]=data[i];
            buffer_ptr++;buffer_ptr%=buffer_sz;
            data[i]=buffer[buffer_ptr];
        }
    }
private:
    std::vector<int> buffer;
    int buffer_ptr;
    int buffer_sz;
};

class AeroLInterleaver
{
public:
    AeroLInterleaver();
    void setSize(int N);
    std::vector<int> &interleave(std::vector<int> &block);
    std::vector<int> &deinterleave(std::vector<int> &block);
    std::vector<int> &deinterleaveMSK(std::vector<int> &block, int blocks);
    std::vector<uint8_t> &deinterleaveMSK_ba(std::vector<int> &block, int blocks);
    std::vector<uint8_t> &deinterleave_ba(std::vector<int> &block, int blocks);


    std::vector<int> &deinterleave(std::vector<int> &block,int cols);//deinterleaves with a fewer number of cols than the block has
private:
    std::vector<int> matrix;
    std::vector<uint8_t> matrix_ba;
    int M;
    int N;
    std::vector<int> interleaverowpermute;
    std::vector<int> interleaverowdepermute;
};

class PreambleDetector
{
public:
    PreambleDetector();
    void setPreamble(std::vector<int> _preamble);
    bool setPreamble(uint64_t bitpreamble,int len);
    bool Update(int val);
private:
    std::vector<int> preamble;
    std::vector<int> buffer;
    int buffer_ptr;
};

class PreambleDetectorPhaseInvariant
{
public:
    PreambleDetectorPhaseInvariant();
    void setPreamble(std::vector<int> _preamble);
    bool setPreamble(uint64_t bitpreamble,int len);
    void setTollerence(int tollerence);
    int Update(int val);
    bool inverted;
private:
    std::vector<int> preamble;
    std::vector<int> buffer;
    int buffer_ptr;
    int tollerence;
};

class OQPSKPreambleDetectorAndAmbiguityCorrection
{
public:
    OQPSKPreambleDetectorAndAmbiguityCorrection();
    bool setPreamble(uint64_t bitpreamble1,uint64_t bitpreamble2,int len);
    void setTollerence(int tollerence);
    int Update(int val);
    bool inverted;

private:
    std::vector<int> preamble1;
    std::vector<int> buffer1;
    std::vector<int> preamble2;
    std::vector<int> buffer2;

    int buffer_ptr;
    int tollerence;

};

class RTChannelDeleaveFECScram
{
public:

    int targetSUSize =0;
    int targetBlocks = 0;

    typedef enum ReturnResult
    {
        OK_Packet=      0b00000001,
        OK_R_Packet=    0b00000011,
        OK_T_Packet=    0b00000101,
        Bad_Packet=     0b00000000,
        Test_Failed=    0b00100000,
        Nothing=        0b00001000,
        FULL=           0b00010000
    } ReturnResult;
    RTChannelDeleaveFECScram()
    {

        block.resize(64*95);//max rows*cols
        leaver.setSize(95);//max cols needed

        lastpacketstate=Nothing;

        jconvolcodec = new JConvolutionalCodec();
        std::vector<uint16_t> polys;
        polys.push_back(109);
        polys.push_back(79);
        jconvolcodec->SetCode(2,7,polys,24);

        resetblockptr();
    }
    ~RTChannelDeleaveFECScram()
    {
        delete jconvolcodec;
    }
    ReturnResult resetblockptr()
    {
        blockptr=0;

        if(lastpacketstate==Test_Failed)
        {
            lastpacketstate=Nothing;
            return Bad_Packet;
        }
        lastpacketstate=Nothing;
        return Nothing;
    }
    void packintobytes()
    {
        infofield.clear();
        int charptr=0;
        uint8_t ch=0;
        for(int h=0;h<(int)deconvol.size();h++)//take one for flushing
        {
            ch|=deconvol[h]*128;
            charptr++;charptr%=8;
            if(charptr==0)
            {
                infofield.push_back(ch);
                ch=0;
            }
            else ch>>=1;
        }
    }


    ReturnResult updateMSK(int bit)
    {
        if(blockptr>=(int)block.size())
        {
            return FULL;
        }
        block[blockptr]=bit;
        blockptr++;

        int ok = 0;

        if(blockptr>=(int)block.size())fprintf(stderr,"FULL\n");

        // for an R Packet we need 5 blocks. For a T Packet check at 8 blocks to find total number
        // of SU's
        bool cont = false;


        if((((blockptr-(64*5))%(64*3))==0) && (blockptr /64 == 5 || blockptr /64 == targetBlocks || blockptr/64 == 11 || blockptr/64 == 50))
        {
            cont = true;
        }

        //test if interleaver length works
        if(cont)//true for R and T packets
        {


            //reset scrambler
            scrambler.reset();

            //deinterleaver
            delBlock = leaver.deinterleaveMSK_ba(block, blockptr/64);

            //decode
            deconvol=jconvolcodec->Decode_soft(delBlock, blockptr);

            //scrambler
            scrambler.update(deconvol);

            //test for R or packet
            if(blockptr==(64*5))
            {
                targetSUSize =0;
                targetBlocks =0;

                //test crc
                bool crcok=crc16.calcusingbitsandcheck(deconvol.data(),8*19);
                if(crcok)
                {

                    //pack into bytes
                    packintobytes();

                    blockptr=block.size();//stop further testing
                    lastpacketstate=OK_R_Packet;

                    return OK_R_Packet;
                }else{

                    return Nothing;

                }
            }

            //Test for T packet
            //test header crc
            bool crcok=crc16.calcusingbitsandcheck(deconvol.data(),8*6);
            if(!crcok)
            {

                lastpacketstate=Bad_Packet;
                return Bad_Packet;
            }
            // good T packet 48 bit header. If we do not yet know how many SU's, go and check this if block nr is 8
            else
            {

                 if(blockptr/64 == 11){

                    // we should be able to peek at the SU after the initial SU and figure out the number of SU's in this
                    // burst

                    std::vector<int> isu(deconvol.begin() + (8*6)+(8*12)*1, deconvol.begin() + (8*6)+(8*12)*1 + 8*12);

                    int bin = 2;
                    bin+= ((isu[0] * 1) + (isu[1] * 2) + (isu[2] * 4) + (isu[3] * 8) + (isu[4] * 16) + (isu[5] * 32));

                    targetSUSize = bin;

                    if(targetSUSize >= 16){
                        targetSUSize = (int)floor(targetSUSize/2) +1;

                    }

                    targetBlocks = ((targetSUSize+1)*3) +2;

                    return Nothing;
                }

                // this should be the target blocks for this T packet
                if(blockptr/64 == targetBlocks)
                {
                    for(int i=0;i<targetSUSize-3;i++)
                    {
                        crcok=crc16.calcusingbitsandcheck(deconvol.data()+(8*6)+(8*12)*i,8*12);

                        if(crcok)
                        {
                            ok++;

                        }

                    }// end SU loop


                    if(ok<=targetSUSize){

                        //pack into bytes
                        packintobytes();
                        if(!infofield.empty()) infofield.pop_back();
                        numberofsus = targetSUSize;
                        blockptr=block.size();//stop further testing
                        lastpacketstate=OK_T_Packet;

                        return OK_T_Packet;


                    }
                }// end target block loop
                // Max Blocks in one loop

                return Nothing;

            }

            //pack into bytes
            packintobytes();
            if(!infofield.empty()) infofield.pop_back();

            blockptr=block.size();//stop further testing
            lastpacketstate=OK_T_Packet;
            return OK_T_Packet;

        }
        return Nothing;
    }


    ReturnResult update(int bit)
    {
        if(blockptr>=(int)block.size())return FULL;
        block[blockptr]=bit;
        blockptr++;


        //test if interleaver length works
        if(((blockptr-(64*5))%(64*3))==0)//true for R and T packets
        {

            //reset scrambler
            scrambler.reset();

            //deinterleaver
            delBlock = leaver.deinterleave_ba(block, blockptr/64);


            //decode
            deconvol=jconvolcodec->Decode_soft(delBlock, blockptr);

            //scrambler
            scrambler.update(deconvol);

            //test for R packet
            if(blockptr==(64*5))
            {
                //test crc
                bool crcok=crc16.calcusingbitsandcheck(deconvol.data(),8*19);
                if(!crcok)
                {
                    lastpacketstate=Test_Failed;
                    return Test_Failed;
                }

                //pack into bytes
                packintobytes();

                blockptr=block.size();//stop further testing
                lastpacketstate=OK_R_Packet;
                return OK_R_Packet;
            }

            //Test for T packet
            //test header crc
            bool crcok=crc16.calcusingbitsandcheck(deconvol.data(),8*6);
            if(!crcok)
            {
                if(blockptr>=(int)block.size())
                {
                    lastpacketstate=Bad_Packet;
                    return Bad_Packet;
                }
                lastpacketstate=Test_Failed;
                return Test_Failed;
            }

            //test all the SU crcs
            numberofsus=1+(blockptr-(64*5))/(64*3);
            for(int i=0;i<numberofsus;i++)
            {
                crcok=crc16.calcusingbitsandcheck(deconvol.data()+(8*6)+(8*12)*i,8*12);
                if(!crcok)
                {
                    if(blockptr>=(int)block.size())
                    {
                        lastpacketstate=Bad_Packet;
                        return Bad_Packet;
                    }
                    lastpacketstate=Test_Failed;
                    return Test_Failed;
                }


            }

            //pack into bytes
            packintobytes();
            if(!infofield.empty()) infofield.pop_back();

            blockptr=block.size();//stop further testing
            lastpacketstate=OK_T_Packet;
            return OK_T_Packet;

        }
        return Nothing;
    }



    std::vector<int> deleaveredblock;
    std::vector<uint8_t> delBlock;
    std::vector<int> deconvol;
    std::vector<int> block;
    int blockptr;
    AeroLInterleaver leaver;
    AeroLScrambler scrambler;
    JConvolutionalCodec *jconvolcodec;
    std::vector<uint8_t> infofield;
    AeroLcrc16 crc16;
    ReturnResult lastpacketstate;
    int numberofsus;
};

/* Callback types for AeroL signals */
typedef void (*aerol_dcd_cb)(bool status, void *user);
typedef void (*aerol_acars_cb)(ACARSItem &acarsitem, void *user);
typedef void (*aerol_decoded_cb)(const uint8_t *data, int len, void *user);
typedef void (*aerol_cassign_cb)(CChannelAssignmentItem &item, void *user);
/* C-channel voice: one 12-byte AMBE frame per call. */
typedef void (*aerol_voice_cb)(const uint8_t *frame, int len, void *user);

class AeroL
{
public:
    enum ChannelType {PChannel,RChannel,TChannel};

    explicit AeroL();
    ~AeroL();

    void setBitRate(double fb);
    void setBurstmode(bool burstmode);
    void setSettings(double fb, bool burstmode);
    void LostSignal()
    {
        cntr=1000000000;
        datacdcountdown=0;
        datacd=false;
        if(dcd_callback) dcd_callback(false, dcd_user);
    }
    void setDoNotDisplaySUs(std::vector<int> &list){donotdisplaysus=list;}
    void processDemodulatedSoftBits(const std::vector<short> &soft_bits);

    /* callbacks */
    void setDCDCallback(aerol_dcd_cb cb, void *user) { dcd_callback = cb; dcd_user = user; }
    void setACARSCallback(aerol_acars_cb cb, void *user) {
        acars_callback = cb;
        acars_user = user;
        parserisu->setACARSCallback(cb, user);
    }
    void setDecodedCallback(aerol_decoded_cb cb, void *user) { decoded_callback = cb; decoded_user = user; }
    void setCChannelAssignmentCallback(aerol_cassign_cb cb, void *user) {
        cassign_callback = cb; cassign_user = user;
    }
    void setVoiceCallback(aerol_voice_cb cb, void *user) {
        voice_callback = cb; voice_user = user;
    }

private:
    std::vector<uint8_t> &Decode(std::vector<short> &bits, bool soft = false);
    std::vector<uint8_t> &DecodeC(std::vector<short> &bits);

    std::vector<short> sbits;
    std::vector<uint8_t> decodedbytes;
    PreambleDetector preambledetector;

    //burstmode really not sure this is used
    PreambleDetector preambledetectorburst;

    // 600 / 1200 MSK phase invariant burst detector
    PreambleDetectorPhaseInvariant mskBurstDetector;

    int muw;
    bool burstmode;
    int ifb;
    RTChannelDeleaveFECScram rtchanneldeleavefecscram;
    //

    //OQPSK
    PreambleDetectorPhaseInvariant preambledetectorphaseinvariantimag;
    PreambleDetectorPhaseInvariant preambledetectorphaseinvariantreal;

    //C Channel OQPSK
    OQPSKPreambleDetectorAndAmbiguityCorrection preambledetectorreal;
    OQPSKPreambleDetectorAndAmbiguityCorrection preambledetectorimag;


    bool useingOQPSK;
    int AERO_SPEC_NumberOfBits;//info only
    int AERO_SPEC_TotalNumberOfBits;//info and header and uw
    int AERO_SPEC_BitsInHeader;
    int realimag;



    std::vector<int> block;
    AeroLInterleaver leaver;
    AeroLScrambler scrambler;

    JConvolutionalCodec *jconvolcodec;

    DelayLine dl1,dl2;

    AeroLcrc16 crc16;

    uint16_t frameinfo;
    uint16_t lastframeinfo;

    std::vector<uint8_t> infofield;


    RISUData risudata;
    ISUData isudata;
    ParserISU *parserisu;

    int datacdcountdown;
    bool datacd;
    int cntr;

    std::vector<int> donotdisplaysus;

    //old static
    int formatid;
    int supfrmaker;
    int framecounter1;
    int framecounter2;
    int gotsync_last;
    int blockcnt;
    int index;
    std::vector<uint8_t> deleaveredBlock;
    std::vector<uint8_t> depuncturedBlock;
    PuncturedCode puncturedCode;

    /* callback pointers */
    aerol_dcd_cb dcd_callback;
    void *dcd_user;
    aerol_acars_cb acars_callback;
    void *acars_user;
    aerol_decoded_cb decoded_callback;
    void *decoded_user;
    aerol_cassign_cb cassign_callback;
    void *cassign_user;
    aerol_voice_cb voice_callback;
    void *voice_user;
};

#endif // AEROL_H
