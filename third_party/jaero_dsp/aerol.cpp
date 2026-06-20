/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#include "aerol.h"
#include <cstring>

/* helper: extract sub-vector (equivalent to QByteArray::mid) */
static std::vector<uint8_t> vec_mid(const std::vector<uint8_t> &v, int pos, int len = -1)
{
    if(pos >= (int)v.size()) return std::vector<uint8_t>();
    if(len < 0) len = (int)v.size() - pos;
    if(pos + len > (int)v.size()) len = (int)v.size() - pos;
    return std::vector<uint8_t>(v.begin() + pos, v.begin() + pos + len);
}

/* helper: extract left n bytes */
static std::vector<uint8_t> vec_left(const std::vector<uint8_t> &v, int n)
{
    return vec_mid(v, 0, n);
}

/* helper: check if vector contains a value */
static bool vec_contains(const std::vector<int> &v, int val)
{
    for(int i=0;i<(int)v.size();i++) if(v[i]==val) return true;
    return false;
}

/* forward decl — parses a 12-byte P-channel C-channel assignment SU */
CChannelAssignmentItem AeroL_CreateCAssignmentItem(const std::vector<uint8_t> &su);

//R channel

int RISUData::findisuitem(RISUItem &anisuitem)
{
    if(anisuitem.SUTYPE>11)return -1;
    if(anisuitem.SUTYPE<1)return -1;
    for(int i=0;i<(int)isuitems.size();i++)
    {
        if((anisuitem.GESID==isuitems[i].GESID)&&(anisuitem.AESID==isuitems[i].AESID)&&(anisuitem.QNO==isuitems[i].QNO)&&(anisuitem.REFNO==isuitems[i].REFNO))return i;
    }
    return -1;
}
void RISUData::deleteoldisuitems()
{
    for(int i=0;i<(int)isuitems.size();i++)
    {
        isuitems[i].count++;
        if(isuitems[i].count>10)//keep last 10 SUs
        {
            isuitems.erase(isuitems.begin()+i);
            i--;
        }
    }
}
bool RISUData::update(std::vector<uint8_t> data)
{
    deleteoldisuitems();//limit number of isu saved

    int byte1=((uint8_t)data[-1+1]);
    int byte2=((uint8_t)data[-1+2]);
    int byte3=((uint8_t)data[-1+3]);
    int byte4=((uint8_t)data[-1+4]);
    int byte5=((uint8_t)data[-1+5]);
    int byte6=((uint8_t)data[-1+6]);

    anisuitem.clear();
    anisuitem.SEQINDICATOR=((byte1&0xF0)>>4);
    anisuitem.SUTYPE=byte1&0x0F;
    anisuitem.QNO=((byte2&0xF0)>>4);
    anisuitem.REFNO=byte2&0x07;
    anisuitem.AESID=byte3<<8*2|byte4<<8*1|byte5<<8*0;
    anisuitem.GESID=byte6;

    int idx=findisuitem(anisuitem);
    if(idx<0)
    {
        isuitems.push_back(anisuitem);
        idx=isuitems.size()-1;
    }
    RISUItem *pisuitem;
    pisuitem=&isuitems[idx];
    pisuitem->count=0;

    int SUTotal=0;
    int SUindex=0;
    switch(anisuitem.SEQINDICATOR)
    {
    case 1:
        SUTotal=1;
        SUindex=0;
        break;
    case 2:
        SUTotal=2;
        SUindex=0;
        break;
    case 3:
        SUTotal=2;
        SUindex=1;
        break;
    case 4:
        SUTotal=3;
        SUindex=0;
        break;
    case 5:
        SUTotal=3;
        SUindex=1;
        break;
    case 6:
        SUTotal=3;
        SUindex=2;
        break;
    default:
        break;
    }
    int BytesInSU=0;
    if((anisuitem.SUTYPE>=1)&&(anisuitem.SUTYPE<=11))BytesInSU=anisuitem.SUTYPE;
    bool SignalingInfoSU=false;
    if(anisuitem.SUTYPE==15)SignalingInfoSU=true;

    int thisnum=11*SUTotal-11+BytesInSU;
    if(thisnum>0)
    {
        if((int)pisuitem->userdata.size()==0)pisuitem->userdata.resize(thisnum);
        if(thisnum<(int)pisuitem->userdata.size())pisuitem->userdata.resize(thisnum);
    }
    if(!SignalingInfoSU)
    {
        for(int i=0-1+7;i<BytesInSU-1+7;i++)pisuitem->userdata[i+11*SUindex+1-7]=data[i];
        pisuitem->filledarray|=(1<<SUindex);
    } else pisuitem->userdata.clear();

    if((SignalingInfoSU)||((pisuitem->filledarray==7)&&(SUTotal==3))||((pisuitem->filledarray==3)&&(SUTotal==2))||((pisuitem->filledarray==1)&&(SUTotal==1)))
    {
        lastvalidisuitem=*pisuitem;
        isuitems.erase(isuitems.begin()+idx);
        return true;
    }

    return false;
}

//

int ISUData::findisuitem71(ISUItem &anisuitem)
{
    if(anisuitem.NOOCTLESTINLASTSSU>8)return -1;
    for(int i=0;i<(int)isuitems.size();i++)
    {
        if((anisuitem.AESID==isuitems[i].AESID)&&(anisuitem.GESID==isuitems[i].GESID)&&(anisuitem.QNO==isuitems[i].QNO)&&(anisuitem.REFNO==isuitems[i].REFNO))return i;
    }
    return -1;
}
int ISUData::findisuitemC0(ISUItem &anisuitem)
{
    if(anisuitem.NOOCTLESTINLASTSSU>8)return -1;
    for(int i=0;i<(int)isuitems.size();i++)
    {
        if(((anisuitem.AESID==isuitems[i].AESID)
            &&(anisuitem.GESID==isuitems[i].GESID)
            &&(anisuitem.SEQNO+1)==isuitems[i].SEQNO)
            &&(anisuitem.QNO==isuitems[i].QNO)
            &&(anisuitem.REFNO==isuitems[i].REFNO))
            return i;
    }
    return -1;
}
void ISUData::deleteoldisuitems()
{
    for(int i=0;i<(int)isuitems.size();i++)
    {
        isuitems[i].count++;
        if(isuitems[i].count>10)//keep last 10 0x71 SUs
        {
            isuitems.erase(isuitems.begin()+i);
            i--;
        }
    }
}
bool ISUData::update(std::vector<uint8_t> data)
{
    missingssu=false;
    assert((int)data.size()>=10);
    uint8_t message=data[0];
    switch(message)
    {
    case AEROTypeP::User_data_ISU_RLS_P_T_channel:
    {

        deleteoldisuitems();//limit number of isu saved

        anisuitem.AESID=((uint8_t)data[1])<<8*2|((uint8_t)data[2])<<8*1|((uint8_t)data[3])<<8*0;
        anisuitem.GESID=(uint8_t)data[4];
        uint8_t val=data[5];
        anisuitem.QNO=(val>>4)&0x0F;
        anisuitem.REFNO=val&0x0F;
        val=data[6];
        anisuitem.SEQNO=val&0x3F;
        val=data[7];
        anisuitem.NOOCTLESTINLASTSSU=(val>>4)&0x0F;
        anisuitem.count=0;
        anisuitem.userdata.clear();
        for(int i=8;i<=9;i++)anisuitem.userdata.push_back(data[i]);

        int idx=findisuitem71(anisuitem);
        if(idx<0)isuitems.push_back(anisuitem);
        else isuitems[idx]=anisuitem;

    }
        break;
    default:
    {
        if((message&0xC0)!=0xC0)break;
        anisuitem.SEQNO=message&0x3F;
        int val=data[1];
        anisuitem.QNO=(val>>4)&0x0F;
        anisuitem.REFNO=val&0x0F;

        int idx=findisuitemC0(anisuitem);
        if(idx<0)
        {
            missingssu=true;
            return false;
        }
        ISUItem *pisuitem;
        pisuitem=&isuitems[idx];

        (pisuitem->SEQNO)--;
        if(pisuitem->SEQNO==0)
        {
            for(int i=2;i<=(pisuitem->NOOCTLESTINLASTSSU+1);i++)pisuitem->userdata.push_back(data[i]);
            lastvalidisuitem=*pisuitem;
            return true;
        }
        else for(int i=2;i<=9;i++)pisuitem->userdata.push_back(data[i]);

    }
        break;
    }

    return false;
}

int ACARSDefragmenter::findfragment(ACARSItem &acarsitem)
{
    for(int idx=0;idx<(int)acarsitemexts.size();idx++)
    {
        ACARSItemext *pitem=&acarsitemexts[idx];
        if(
                (acarsitem.PLANEREG==pitem->anacarsitem.PLANEREG)&&
                (acarsitem.LABEL==pitem->anacarsitem.LABEL)&&
                (acarsitem.MODE==pitem->anacarsitem.MODE)&&
                (acarsitem.isuitem.AESID==pitem->anacarsitem.isuitem.AESID)&&
                (acarsitem.isuitem.GESID==pitem->anacarsitem.isuitem.GESID)&&
                (pitem->anacarsitem.moretocome)
                )
        {

            if(acarsitem.TAK!=pitem->anacarsitem.TAK)
            {
                continue;
            }

            uint8_t expnewbi=(((pitem->anacarsitem.BI+1)-'A')%26)+'A';
            if(expnewbi==acarsitem.BI)
            {
                return idx;
            }

        }
    }
    return -1;
}
bool ACARSDefragmenter::defragment(ACARSItem &acarsitem)
{

    //flush old frags
    for(int i=0;i<(int)acarsitemexts.size();i++)
    {
        acarsitemexts[i].count++;
        if(acarsitemexts[i].count>30)
        {
            acarsitemexts.erase(acarsitemexts.begin()+i);
            i--;
        }
    }

    //if cant find frag then do nothing else push a new frag
    int idx=findfragment(acarsitem);
    if(idx<0)
    {
        if(!acarsitem.moretocome)return true;
        anacarsitemext.anacarsitem=acarsitem;
        anacarsitemext.count=0;
        acarsitemexts.push_back(anacarsitemext);
        return false;
    }

    //update frag
    ACARSItemext *polditem=&acarsitemexts[idx];
    polditem->count=0;
    polditem->anacarsitem.BI=acarsitem.BI;
    polditem->anacarsitem.message.insert(polditem->anacarsitem.message.end(), acarsitem.message.begin(), acarsitem.message.end());
    polditem->anacarsitem.moretocome=acarsitem.moretocome;

    //more to come then do nothing
    if(acarsitem.moretocome)return false;

    //else return defragmented item with BI equal to last acars message
    acarsitem=polditem->anacarsitem;
    acarsitemexts.erase(acarsitemexts.begin()+idx);
    return true;


}

ParserISU::ParserISU()
{
    downlink=false;
    acars_callback=NULL;
    acars_user=NULL;
}
bool ParserISU::parse(ISUItem &isuitem)
{

    if(isuitem.AESID==0)
    {
        return false;
    }
    std::vector<int> parities;
    std::vector<uint8_t> textish;
    for(int i=0;i<(int)isuitem.userdata.size();i++)
    {
        int byte=(uint8_t)isuitem.userdata[i];
        int parity=0;for(int j=0;j<8;j++)if((byte>>j)&0x01)parity++;
        parity&=1;
        if(parity)parities.push_back(0x01);
        else parities.push_back(0x00);
        byte&=0x7F;
        textish.push_back((uint8_t)byte);
    }

    bool isacars = false;

    if(((int)isuitem.userdata.size()>16)
            &&((uint8_t)isuitem.userdata[0])==0xFF
            &&((uint8_t)isuitem.userdata[1])==0xFF
            &&(((uint8_t)isuitem.userdata[15])==0x83 || ((uint8_t)isuitem.userdata[15])==0x02 ))
        isacars = true;

    if(isacars)
    {

        //fill in header info
        anacarsitem.clear();
        anacarsitem.downlink=downlink;
        anacarsitem.isuitem=isuitem;
        uint8_t byte=((uint8_t)isuitem.userdata[3]);
        anacarsitem.MODE=byte&0x7F;
        anacarsitem.TAK=((uint8_t)textish[11]);
        anacarsitem.LABEL.push_back(((uint8_t)textish[12]));
        anacarsitem.LABEL.push_back(((uint8_t)textish[13]));
        anacarsitem.BI=((uint8_t)textish[14]);
        if(((uint8_t)isuitem.userdata[15])==0x02)anacarsitem.hastext=true;
        if(((uint8_t)isuitem.userdata[isuitem.userdata.size()-1-3])==0x97){
            anacarsitem.moretocome=true;
        }
        for(int k=4;k<4+7;k++)
        {
            byte=((uint8_t)isuitem.userdata[k]);
            byte&=0x7F;
            if(!parities[k])
            {
               return false;
            }
            anacarsitem.PLANEREG.push_back((uint8_t)byte);


        }


        //fill in message
        if(anacarsitem.hastext)
        {
            for(int k=16;k<(int)isuitem.userdata.size()-1-3;k++)
            {
                byte=((uint8_t)isuitem.userdata[k]);
                byte&=0x7F;
                if(!parities[k])
                {
                   return false;
                }

                anacarsitem.message.push_back((uint8_t)byte);

            }
        }

        //mark as valid
        anacarsitem.valid=true;

       //send acars message if fully defraged
        if(acarsdefragmenter.defragment(anacarsitem))
        {
            /* remove prepending dots from PLANEREG */
            int i=0;
            while((i<(int)anacarsitem.PLANEREG.size())&&(anacarsitem.PLANEREG[i]=='.'))i++;
            if(i>0) anacarsitem.PLANEREG.erase(anacarsitem.PLANEREG.begin(), anacarsitem.PLANEREG.begin()+i);

            if(acars_callback) acars_callback(anacarsitem, acars_user);
        }

        return true;
    }

    //not acars message so say so and return the raw hex data as the message
    anacarsitem.clear();
    anacarsitem.downlink=downlink;
    anacarsitem.isuitem=isuitem;
    anacarsitem.message.clear();
    anacarsitem.nonacars=true;
    /* store raw userdata bytes as the message */
    anacarsitem.message = isuitem.userdata;
    anacarsitem.valid=true;

    /* remove prepending dots from PLANEREG */
    int i=0;
    while((i<(int)anacarsitem.PLANEREG.size())&&(anacarsitem.PLANEREG[i]=='.'))i++;
    if(i>0) anacarsitem.PLANEREG.erase(anacarsitem.PLANEREG.begin(), anacarsitem.PLANEREG.begin()+i);

    if(acars_callback) acars_callback(anacarsitem, acars_user);

    return true;

}

AeroLInterleaver::AeroLInterleaver()
{
    M=64;

    interleaverowpermute.resize(M);
    interleaverowdepermute.resize(M);

    //this is 1-1 and onto
    for(int i=0;i<M;i++)
    {
        interleaverowpermute[(i*27)%M]=i;
        interleaverowdepermute[i]=(i*27)%M;
    }
    setSize(6);
}
void AeroLInterleaver::setSize(int _N)
{
    if(_N<1)return;
    N=_N;
    matrix.resize(M*N);
    matrix_ba.resize(M*N);
    for(int a = 0; a < (int)matrix_ba.size(); a++)
    {
        matrix_ba[a] = 0;
    }
}
std::vector<int> &AeroLInterleaver::interleave(std::vector<int> &block)
{
    assert((int)block.size()==(M*N));
    int k=0;
    for(int i=0;i<M;i++)
    {
        for(int j=0;j<N;j++)
        {
            int entry=interleaverowpermute[i]+M*j;
            assert(entry<(int)block.size());
            assert(k<(int)matrix.size());
            matrix[k]=block[entry];
            k++;
        }
    }
    return matrix;
}
std::vector<int> &AeroLInterleaver::deinterleave(std::vector<int> &block)
{
    assert((int)block.size()==(M*N));
    int k=0;
    for(int j=0;j<N;j++)
    {
        for(int i=0;i<M;i++)
        {
            int entry=interleaverowdepermute[i]*N+j;
            assert(entry<(int)block.size());
            assert(k<(int)matrix.size());
            matrix[k]=block[entry];
            k++;
        }
    }
    return matrix;
}
std::vector<int> &AeroLInterleaver::deinterleave(std::vector<int> &block,int cols)
{
    assert(cols<=N);
    assert((int)block.size()>=(M*cols));
    int k=0;
    for(int j=0;j<cols;j++)
    {
        for(int i=0;i<M;i++)
        {
            int entry=interleaverowdepermute[i]*cols+j;
            assert(entry<(int)block.size());
            assert(k<(int)matrix.size());
            matrix[k]=block[entry];
            k++;
        }
    }
    return matrix;
}

std::vector<uint8_t> &AeroLInterleaver::deinterleave_ba(std::vector<int> &block,int cols)
{

    // default to MSK settings if zero
    if(cols==0){
        cols = N;
    }
    assert(cols<=N);
    assert((int)block.size()>=(M*cols));
    int k=0;
    for(int j=0;j<cols;j++)
    {
        for(int i=0;i<M;i++)
        {
            int entry=interleaverowdepermute[i]*cols+j;
            assert(entry<(int)block.size());
            assert(k<(int)matrix_ba.size());
            matrix_ba[k]=(uint8_t)block[entry];
            k++;
        }
    }
    return matrix_ba;
}


std::vector<int> &AeroLInterleaver::deinterleaveMSK(std::vector<int> &block, int blocks)
{
    assert((int)block.size()>=(M*blocks));

    // we need to first deinterleave 5 cols for the first block then 3 cols for the remaining blocks
    int k=0;

    for(int j=0;j<5;j++)
    {
        for(int i=0;i<M;i++)
        {
            int entry=interleaverowdepermute[i]*5+j;
            assert(entry<5*M);
            assert(k<(int)matrix.size());
            matrix[k]=block[entry];
            k++;
        }

    }

    int procblocks = 5;
    while (k < blocks *M){


        for(int j=0;j<3;j++)
        {
            for(int i=0;i<M;i++)
            {
                int entry=(M*procblocks) + (interleaverowdepermute[i]*3+j);
                assert(entry < (int)block.size());
                assert(k<(int)matrix.size());
                matrix[k]=block[entry];
                k++;

            }
        }
        procblocks+=3;
    }

    return matrix;
}


std::vector<uint8_t> &AeroLInterleaver::deinterleaveMSK_ba(std::vector<int> &block, int blocks)
{
    assert((int)block.size()>=(M*blocks));

    // we need to first deinterleave 5 cols for the first block then 3 cols for the remaining blocks
    int k=0;

    for(int j=0;j<5;j++)
    {
        for(int i=0;i<M;i++)
        {
            int entry=interleaverowdepermute[i]*5+j;
            assert(entry<5*M);
            assert(k<(int)matrix_ba.size());
            matrix_ba[k]=(char)block[entry];
            k++;
        }

    }


    int procblocks = 5;

    while (k < blocks *M){


        for(int j=0;j<3;j++)
        {
            for(int i=0;i<M;i++)
            {
                int entry=(M*procblocks) + (interleaverowdepermute[i]*3+j);
                assert(entry < (int)block.size());
                assert(k<(int)matrix_ba.size());
                matrix_ba[k]=(char)block[entry];
                k++;

            }
        }
        procblocks+=3;
    }


    return matrix_ba;
}


PreambleDetector::PreambleDetector()
{
    preamble.resize(1);
    buffer.resize(1);
    buffer_ptr=0;
}
void PreambleDetector::setPreamble(std::vector<int> _preamble)
{
    preamble=_preamble;
    if((int)preamble.size()<1)preamble.resize(1);
    buffer.assign(preamble.size(),0);
    buffer_ptr=0;
}
bool PreambleDetector::setPreamble(uint64_t bitpreamble,int len)
{
    if(len<1||len>64)return false;
    preamble.clear();
    for(int i=len-1;i>=0;i--)
    {
        if((bitpreamble>>i)&1)preamble.push_back(1);
        else preamble.push_back(0);
    }
    if((int)preamble.size()<1)preamble.resize(1);
    buffer.assign(preamble.size(),0);
    buffer_ptr=0;
    return true;
}
bool PreambleDetector::Update(int val)
{
    for(int i=0;i<((int)buffer.size()-1);i++)buffer[i]=buffer[i+1];
    buffer[buffer.size()-1]=val;
    if(buffer==preamble){buffer.assign(buffer.size(),0);return true;}
    return false;
}

PreambleDetectorPhaseInvariant::PreambleDetectorPhaseInvariant()
{
    inverted=false;
    preamble.resize(1);
    buffer.resize(1);
    buffer_ptr=0;
    tollerence=0;
}
void PreambleDetectorPhaseInvariant::setPreamble(std::vector<int> _preamble)
{
    preamble=_preamble;
    if((int)preamble.size()<1)preamble.resize(1);
    buffer.assign(preamble.size(),0);
    buffer_ptr=0;
}
bool PreambleDetectorPhaseInvariant::setPreamble(uint64_t bitpreamble,int len)
{
    if(len<1||len>64)return false;
    preamble.clear();
    for(int i=len-1;i>=0;i--)
    {
        if((bitpreamble>>i)&1)preamble.push_back(1);
        else preamble.push_back(0);
    }
    if((int)preamble.size()<1)preamble.resize(1);
    buffer.assign(preamble.size(),0);
    buffer_ptr=0;
    return true;
}
int PreambleDetectorPhaseInvariant::Update(int val)
{
    assert((int)buffer.size()==(int)preamble.size());
    int xorsum=0;
    for(int i=0;i<((int)buffer.size()-1);i++)
    {
        buffer[i]= buffer[i+1];
        xorsum+=buffer[i]^preamble[i];
    }
    xorsum+=val^preamble[buffer.size()-1];
    buffer[buffer.size()-1]=val;
    if(xorsum>=((int)buffer.size()-tollerence)){


        inverted=true;
        return true;
    }
    if(xorsum<=tollerence){
        inverted=false;

        return true;
    }
    return false;
}
void PreambleDetectorPhaseInvariant::setTollerence(int _tollerence)
{
    tollerence=_tollerence;
}

/* for C Channels*/
OQPSKPreambleDetectorAndAmbiguityCorrection::OQPSKPreambleDetectorAndAmbiguityCorrection()
{
    inverted=false;
    preamble1.resize(1);
    buffer1.resize(1);
    preamble2.resize(1);
    buffer2.resize(1);

    buffer_ptr=0;
    tollerence=0;
}

bool OQPSKPreambleDetectorAndAmbiguityCorrection::setPreamble(uint64_t bitpreamble1,uint64_t bitpreamble2,int len)
{
    if(len<1||len>64)return false;
    preamble1.clear();
    preamble2.clear();

    for(int i=len-1;i>=0;i--)
    {
        if((bitpreamble1>>i)&1)preamble1.push_back(1);
        else preamble1.push_back(0);

        if((bitpreamble2>>i)&1)preamble2.push_back(1);
        else preamble2.push_back(0);
    }
    if((int)preamble1.size()<1)preamble1.resize(1);
    buffer1.assign(preamble1.size(),0);

    if((int)preamble2.size()<1)preamble2.resize(1);
    buffer2.assign(preamble2.size(),0);

    buffer_ptr=0;


    return true;
}
int OQPSKPreambleDetectorAndAmbiguityCorrection::Update(int val)
{
    assert((int)buffer1.size()==(int)preamble1.size());
    assert((int)buffer2.size()==(int)preamble2.size());

    /* check the first buffer and preamble */
    int xorsum=0;
    for(int i=0;i<((int)buffer1.size()-1);i++)
    {
        buffer1[i]=buffer1[i+1];
        xorsum+=buffer1[i]^preamble1[i];
    }
    xorsum+=val^preamble1[buffer1.size()-1];
    buffer1[buffer1.size()-1]=val;
    if(xorsum>=((int)buffer1.size()-tollerence)){

        inverted=true;

        return true;
    }
    if(xorsum<=tollerence){

        inverted=false;

        return true;
    }

    xorsum=0;
    for(int i=0;i<((int)buffer2.size()-1);i++)
    {
        buffer2[i]=buffer2[i+1];
        xorsum+=buffer2[i]^preamble2[i];
    }
    xorsum+=val^preamble2[buffer2.size()-1];
    buffer2[buffer2.size()-1]=val;
    if(xorsum>=((int)buffer2.size()-tollerence)){

        inverted=true;
        return true;
    }
    if(xorsum<=tollerence){

        inverted=false;
        return true;
    }


   return false;
}
void OQPSKPreambleDetectorAndAmbiguityCorrection::setTollerence(int _tollerence)
{
    tollerence=_tollerence;
}



AeroL::AeroL()
{

    cntr=1000000000;
    formatid=0;
    supfrmaker=0;
    framecounter1=0;
    framecounter2=0;
    gotsync_last=0;
    blockcnt=-1;

    dcd_callback=NULL;
    dcd_user=NULL;
    acars_callback=NULL;
    acars_user=NULL;
    decoded_callback=NULL;
    decoded_user=NULL;
    cassign_callback=NULL;
    cassign_user=NULL;
    voice_callback=NULL;
    voice_user=NULL;

    //install parser
    parserisu = new ParserISU();

    donotdisplaysus.clear();

    cntr=1000000000;
    datacdcountdown=0;
    datacd=false;

    sbits.reserve(1000);
    decodedbytes.reserve(1000);

    //new viterbi decoder
    jconvolcodec = new JConvolutionalCodec();
    std::vector<uint16_t> polys;
    polys.push_back(109);
    polys.push_back(79);
    jconvolcodec->SetCode(2,7,polys,24);



    dl1.setLength(12);//delay for decode encode BER check
    dl2.setLength(576-6);//delay's data to next frame

    preambledetector.setPreamble(3780831379LL,32);//0x3780831379,0b11100001010110101110100010010011

    //I	1010 1011 0011 0111 0110 1001 0011 1000 1011 1100 1010 0011 0000 = 0xAB376938BCA30 hex = 3012071630031408 decimal
    //Q	0000 1100 0101 0011 1101 0001 1100 1001 0110 1110 1100 1101 0101 = 0xC53D1C96ECD5 hex = 216866263330005 decimal

    // doual preamble detectors for C Channel
    preambledetectorreal.setPreamble(216866263330005LL,3012071630031408LL,52);
    preambledetectorimag.setPreamble(216866263330005LL,3012071630031408LL,52);

    index = 0;

    //Preamble detector for OQPSK
    preambledetectorphaseinvariantimag.setPreamble(3780831379LL,32);//0x3780831379,0b11100001010110101110100010010011
    preambledetectorphaseinvariantreal.setPreamble(3780831379LL,32);//0x3780831379,0b11100001010110101110100010010011

    // MSK burst preamble detector
    mskBurstDetector.setPreamble(3780831379LL,32);//0x3780831379,0b11100001010110101110100010010011

    //Preamble for start of burst
    std::vector<int> pre;
    pre.push_back(0x11);
    pre.push_back(0x07);
    pre.push_back(0x42);
    pre.push_back(0x00);
    pre.push_back(0x00);
    pre.push_back(0x13);
    pre.push_back(0x09);
    preambledetectorburst.setPreamble(pre);

    setSettings(1200,false);

}

void AeroL::setBitRate(double fb)
{
    setSettings(fb,burstmode);
}

void AeroL::setBurstmode(bool _burstmode)
{
    setSettings(ifb,_burstmode);
}

void AeroL::setSettings(double fb, bool _burstmode)
{
    isudata.reset();
    risudata.reset();
    burstmode=_burstmode;

    if(burstmode)
    {
        preambledetectorphaseinvariantimag.setTollerence(4);
        preambledetectorphaseinvariantreal.setTollerence(4);
        preambledetectorimag.setTollerence(0);
        preambledetectorreal.setTollerence(0);
        mskBurstDetector.setTollerence(4);
    }
    else
    {
        preambledetectorphaseinvariantimag.setTollerence(0);
        preambledetectorphaseinvariantreal.setTollerence(0);
        preambledetectorimag.setTollerence(6);
        preambledetectorreal.setTollerence(6);
        mskBurstDetector.setTollerence(0);
    }
    ifb=(int)round(fb);
    switch(ifb)
    {
    case 600:
        leaver.setSize(6);//9 for 1200 bps, 6 for 600 bps
        block.resize(6*64);
        dl2.setLength(576-6);//delay's data to next frame
        AERO_SPEC_NumberOfBits=1152;
        AERO_SPEC_BitsInHeader=16;
        AERO_SPEC_TotalNumberOfBits=AERO_SPEC_BitsInHeader+AERO_SPEC_NumberOfBits+32;
        useingOQPSK=false;
        break;
    case 1200:
        leaver.setSize(9);//9 for 1200 bps, 6 for 600 bps
        block.resize(9*64);
        dl2.setLength(576-6);//delay's data to next frame
        AERO_SPEC_NumberOfBits=1152;
        AERO_SPEC_BitsInHeader=16;
        AERO_SPEC_TotalNumberOfBits=AERO_SPEC_BitsInHeader+AERO_SPEC_NumberOfBits+32;
        useingOQPSK=false;
        break;
    case 8400:
        leaver.setSize(4);//4 rows at 64 bits each
        block.resize(4*64); // 1 deleaver block
        deleaveredBlock.resize(16*4*64);
        dl2.setLength(2714-6);//delay's data to next frame
        depuncturedBlock.reserve(5460);
        AERO_SPEC_NumberOfBits=4096;
        AERO_SPEC_BitsInHeader=0;//178 dummy bits
        AERO_SPEC_TotalNumberOfBits=AERO_SPEC_BitsInHeader+AERO_SPEC_NumberOfBits;
        useingOQPSK=true;
        break;
    case 10500:
        leaver.setSize(78);//78 for 10.5k
        block.resize(78*64);
        dl2.setLength(4992-6);//delay's data to next frame
        AERO_SPEC_NumberOfBits=4992;
        AERO_SPEC_BitsInHeader=16+178;//178 dummy bits
        AERO_SPEC_TotalNumberOfBits=AERO_SPEC_BitsInHeader+AERO_SPEC_NumberOfBits+64;
        useingOQPSK=true;
        break;
    default:
        leaver.setSize(9);//9 for 1200 bps, 6 for 600 bps
        block.resize(9*64);
        AERO_SPEC_NumberOfBits=1152;
        AERO_SPEC_BitsInHeader=16;
        useingOQPSK=false;
        break;
    }

    if(burstmode){

        if(useingOQPSK){
            AERO_SPEC_TotalNumberOfBits=ifb;//1 sec countdown for burst modes
        }
        else{
            AERO_SPEC_TotalNumberOfBits=ifb*3;//3 sec countdown for burst modes
        }
    }

}

AeroL::~AeroL()
{
    delete parserisu;
    delete jconvolcodec;
}

std::vector<uint8_t> &AeroL::Decode(std::vector<short> &bits, bool soft)//0 bit --> oldest bit
{
    decodedbytes.clear();

    uint16_t bit=0;
    uint16_t soft_bit=0;

    for(int i=0;i<(int)bits.size();i++)
    {

        if(soft)
        {
            if(((uint8_t)bits[i])>=128)bit=1;
            else bit=0;
        }
         else
         {
            bit=bits[i];
         }
        soft_bit=bits[i];

        //for burst mode to allow tolerance of UW
        if(bits[i]<0)
        {
            muw=0;
            continue;
        }
        if(muw<100000)muw++;

        //Preamble detector and ambiguity corrector for OQPSK
        int gotsync;
        if(useingOQPSK)
        {
            realimag++;realimag%=2;
            if(realimag)
            {

                if(cntr > AERO_SPEC_NumberOfBits-68 || cntr <= 0 ||!datacd)
                {
                gotsync=preambledetectorphaseinvariantimag.Update(bit);
                if(!gotsync_last)
                {
                    gotsync_last=gotsync;
                    gotsync=0;
                } else gotsync_last=0;
                }else{
                    gotsync = false;
                    gotsync_last = false;
                }
            }
             else
             {
                if(cntr > AERO_SPEC_NumberOfBits-68 || cntr <= 0 ||!datacd)
                {
                gotsync=preambledetectorphaseinvariantreal.Update(bit);
                if(!gotsync_last)
                {
                    gotsync_last=gotsync;
                    gotsync=0;
                } else gotsync_last=0;

                }else{
                    gotsync = false;
                    gotsync_last = false;
                }
             }

            //for 10500 UW should be about 80 samples after start of packet signal from demodulator if not we have a false positive
            if(gotsync&&burstmode)
            {
                if(ifb==10500&&(abs(muw-80)>150))
                {
                    gotsync=false;
                }
            }

            if(realimag)
            {
                if(preambledetectorphaseinvariantimag.inverted)
                {
                    bit=1-bit;

                    if(soft_bit > 128){
                         soft_bit = 255-soft_bit;
                    } else if (soft_bit < 128){
                        soft_bit = 255-soft_bit;
                    }


                }

            }
            else
            {
                if(preambledetectorphaseinvariantreal.inverted)
                {
                    bit=1-bit;

                    if(soft_bit > 128)
                    {
                         soft_bit = 255-soft_bit;
                    }
                     else if (soft_bit < 128)
                     {
                        soft_bit = 255-soft_bit;
                     }
                }
            }

        } //non 10500 burst mode use a phase invariant preamble detector
         else if(burstmode)
         {

            bool inverted = mskBurstDetector.inverted;

            gotsync=mskBurstDetector.Update(bit);

            if( muw > 250 && gotsync)
            {

                if(inverted != mskBurstDetector.inverted)
                {
                    mskBurstDetector.inverted = inverted;
                }
                gotsync = false;

            }

            if(mskBurstDetector.inverted)
            {

                bit=1-bit;

                if(soft_bit > 128)
                {
                     soft_bit = 255-soft_bit;
                }
                 else if (soft_bit < 128)
                 {
                    soft_bit = 255-soft_bit;
                 }
            }
         }
          else
          {
            gotsync=preambledetector.Update(bit);
          }

        if(cntr<1000000000)cntr++;
        if(cntr<16)
        {
            if(cntr==0)
            {
                frameinfo=bit;
                infofield.clear();
                if(burstmode)//for R and T channels there are no headers so just fill in a dummy one
                {
                    formatid=1;
                    supfrmaker=0;
                    framecounter1=0;
                    framecounter2=0;
                    cntr=16;

                    if(rtchanneldeleavefecscram.resetblockptr()==RTChannelDeleaveFECScram::Bad_Packet)
                    {
                        /* Bad R/T Packet */
                    }
                }
            }
            else
            {
                frameinfo<<=1;
                frameinfo|=bit;
            }
        }
        if(cntr==15)
        {

            //delay frame info by one frame needed as viterbi decoder and delayline make 1 frame delay
            uint16_t tval=frameinfo;
            frameinfo=lastframeinfo;
            lastframeinfo=tval;

            formatid=(frameinfo>>12)&0x000F;
            supfrmaker=(frameinfo>>8)&0x000F;
            framecounter1=(frameinfo>>4)&0x000F;
            framecounter2=(frameinfo>>0)&0x000F;

        }
        if(cntr>=16)
        {

            //do rt decoding
            if(burstmode)
            {

                RTChannelDeleaveFECScram::ReturnResult result;

                if(useingOQPSK)
                {
                    if(soft)result = rtchanneldeleavefecscram.update(soft_bit);
                     else result = rtchanneldeleavefecscram.update(bit);
                }
                 else
                 {
                    if(soft)result = rtchanneldeleavefecscram.updateMSK(soft_bit);
                     else result = rtchanneldeleavefecscram.updateMSK(bit);
                 }

                switch(result)
                {
                case RTChannelDeleaveFECScram::OK_R_Packet:
                {

                    //processing R channel packets
                    using namespace AEROTypeR;
                    MessageType message=(MessageType)((uint8_t)rtchanneldeleavefecscram.infofield[2]);
                    if((((uint8_t)rtchanneldeleavefecscram.infofield[1])&0x08)==0x08)message=User_data_ISU_SSU_R_channel;

                    switch(message)
                    {
                    case User_data_ISU_SSU_R_channel:

                        if(risudata.update(vec_left(rtchanneldeleavefecscram.infofield, 17)))
                        {
                            parserisu->downlink=burstmode;
                            parserisu->parse(risudata.lastvalidisuitem);//emits signals if it find valid acars data or errors
                        }

                        break;
                    default:
                        break;
                    }

                    /* emit decoded frame data via callback */
                    if(decoded_callback && !rtchanneldeleavefecscram.infofield.empty())
                    {
                        decoded_callback(rtchanneldeleavefecscram.infofield.data(), (int)rtchanneldeleavefecscram.infofield.size(), decoded_user);
                    }

                   }
                    break;
                case RTChannelDeleaveFECScram::OK_T_Packet:
                {

                    using namespace AEROTypeT;

                    for(int k=0;k<rtchanneldeleavefecscram.numberofsus;k++)
                    {

                        MessageType message=(MessageType)((uint8_t)rtchanneldeleavefecscram.infofield[6+k*12]);
                        if((message&0xC0)==0xC0)message=User_data_ISU_SSU_T_channel;
                        switch(message)
                        {
                        case Fill_in_signal_unit:
                            break;
                        case User_data_ISU_RLS_T_channel:
                            isudata.update(vec_mid(rtchanneldeleavefecscram.infofield,6+k*12,10));
                            break;
                        case User_data_ISU_SSU_T_channel:
                            if(isudata.update(vec_mid(rtchanneldeleavefecscram.infofield,6+k*12,10)))
                            {
                                parserisu->downlink=burstmode;
                                parserisu->parse(isudata.lastvalidisuitem);//emits signals if it find valid acars data or errors
                            }
                            break;

                        }

                    }// done with this burst

                    /* emit decoded frame data via callback */
                    if(decoded_callback && !rtchanneldeleavefecscram.infofield.empty())
                    {
                        decoded_callback(rtchanneldeleavefecscram.infofield.data(), (int)rtchanneldeleavefecscram.infofield.size(), decoded_user);
                    }

                }
                    break;
                case RTChannelDeleaveFECScram::Bad_Packet:
                    break;
                default:
                    break;
                }

            }

             else
             {

                // its a p channel

                //fill block
                if(cntr==16)blockcnt=-1;
                int idx=(cntr-AERO_SPEC_BitsInHeader)%block.size();
                if(idx<0)idx=0;//for dummy bits drop
                block[idx]=soft_bit;

                if(idx==((int)block.size()-1))//block is now filled
                {

                    blockcnt++;

                    //deinterleaver
                    std::vector<uint8_t> deleaveredblockBA=leaver.deinterleave_ba(block, 0);

                    std::vector<int> deconvol=jconvolcodec->Decode_Continuous(deleaveredblockBA);

                   //delay line for frame alignment for non burst modes. This is needed for the scrambler
                    dl2.update(deconvol);

                    //scrambler
                    scrambler.update(deconvol);

                    //pack the bits into bytes
                    int charptr=0;
                    uint8_t ch=0;
                    for(int h=0;h<(int)deconvol.size();h++)
                    {
                        ch|=deconvol[h]*128;
                        charptr++;charptr%=8;
                        if(charptr==0)
                        {
                            infofield.push_back(ch);//actual data of information field
                            ch=0;
                        }
                        else ch>>=1;
                    }

                    if((cntr-AERO_SPEC_BitsInHeader)==(AERO_SPEC_NumberOfBits-1))//frame is done when this is true
                    {

                        //run through all SUs and check CRCs
                        char *infofieldptr=(char*)infofield.data();
                        for(int k=0;k<(int)infofield.size()/12;k++)
                        {
                            uint16_t crc_calc=crc16.calcusingbytes(&infofieldptr[k*12],12-2);
                            uint16_t crc_rec=(((uint8_t)infofield[k*12+12-1])<<8)|((uint8_t)infofield[k*12+12-2]);

                            if((!crc_rec)&&(crc_calc!=crc_rec))
                            {
                                int tsum=0;
                                for(int ii=0; ii<(12-2); ii++)tsum+=(uint8_t)infofieldptr[k*12+ii];
                                if(tsum==0)crc_calc=0;//some sus are just zeros
                            }

                            //keep track of the DCD for non burst modes
                            if(crc_calc==crc_rec){if(datacdcountdown<12)datacdcountdown+=2;}
                            else {if(datacdcountdown>0)datacdcountdown-=3;}
                            if(!datacd&&datacdcountdown>2)
                            {
                                datacd=true;
                                if(dcd_callback) dcd_callback(datacd, dcd_user);
                            }

                            if(crc_calc==crc_rec)
                            {

                                //processing P channel packets
                                using namespace AEROTypeP;
                                MessageType message=(MessageType)((uint8_t)infofield[k*12]);
                                switch(message)
                                {
                                case User_data_ISU_RLS_P_T_channel:
                                    isudata.update(vec_mid(infofield,k*12,10));
                                    break;
                                case C_channel_assignment_distress:
                                case C_channel_assignment_flight_safety:
                                case C_channel_assignment_other_safety:
                                case C_channel_assignment_non_safety:
                                    if(cassign_callback) {
                                        CChannelAssignmentItem item =
                                            AeroL_CreateCAssignmentItem(vec_mid(infofield,k*12,12));
                                        item.type = (uint8_t)message;
                                        cassign_callback(item, cassign_user);
                                    }
                                    break;
                                default:
                                    if((message&0xC0)==0xC0)
                                    {

                                        if(isudata.update(vec_mid(infofield,k*12,10)))
                                        {
                                            parserisu->downlink=burstmode;
                                            parserisu->parse(isudata.lastvalidisuitem);//emits signals if it find valid acars data or errors
                                        }

                                    }
                                    break;
                                }



                            }

                        }

                        /* emit decoded frame data via callback */
                        if(decoded_callback && !infofield.empty())
                        {
                            decoded_callback(infofield.data(), (int)infofield.size(), decoded_user);
                        }

                    }

                }


             }


        }


        if(gotsync)
        {

            if(!burstmode)
            {
                if(cntr+1!=AERO_SPEC_TotalNumberOfBits)
                {
                    isudata.reset();
                }
            }

            cntr=-1;

            //got a signal
            datacd=true;
            datacdcountdown=12;
            if(dcd_callback) dcd_callback(datacd, dcd_user);

            scrambler.reset();
        }

        if(cntr+1==AERO_SPEC_TotalNumberOfBits)
        {
            scrambler.reset();
            cntr=-1;

            if(burstmode)
            {

               //end of signal. stop processing and send dcd low
                cntr=1000000000;//stop processing
                datacd=false;
                datacdcountdown=0;
                if(dcd_callback) dcd_callback(datacd, dcd_user);
                return decodedbytes;
            }
        }


    }

    if((!datacd)&&(!burstmode))
    {
        decodedbytes.clear();
    }

    return decodedbytes;
}

void AeroL::processDemodulatedSoftBits(const std::vector<short> &soft_bits)
{

    sbits.clear();

    sbits.insert(sbits.end(), soft_bits.begin(), soft_bits.end());

    if(this->ifb == 8400)
    {
       DecodeC(sbits);
    }else
    {
       Decode(sbits, true);
    }

}

CChannelAssignmentItem AeroL_CreateCAssignmentItem(const std::vector<uint8_t> &su)
{
    CChannelAssignmentItem item;

    item.type=(uint8_t)su[0];

    item.AESID=((uint8_t)su[-1+2])<<8*2|((uint8_t)su[-1+3])<<8*1|((uint8_t)su[-1+4])<<8*0;
    item.GESID=su[-1+5];

    int byte7=((uint8_t)su[-1+7]);
    int byte8=((uint8_t)su[-1+8]);
    int byte9=((uint8_t)su[-1+9]);
    int byte10=((uint8_t)su[-1+10]);

    int channel_rx=((((byte7&0x7F)<<8)&0xFF00)|(byte8&0x00FF));
    int channel_tx=((((byte9&0x7F)<<8)&0xFF00)|(byte10&0x00FF));
    item.receive_freq=(((double)channel_rx)*0.0025)+1510.0;
    item.transmit_freq=(((double)channel_tx)*0.0025)+1611.5;
    if(byte7&0x80)item.receive_spotbeam=true;
    if(byte9&0x80)item.transmit_spotbeam=true;

    return item;
}

std::vector<uint8_t> &AeroL::DecodeC(std::vector<short> &bits)
{

    decodedbytes.clear();



    uint16_t bit=0;
    uint16_t soft_bit=0;

    for(int i=0;i<(int)bits.size();i++)
    {

        // hard bits for preamble
        if(((uint8_t)bits[i])>=128)bit=1;
        else bit=0;
        soft_bit=bits[i];
        int gotsync = 0;

        realimag++;realimag%=2;
        if(realimag)
        {

            if(cntr > AERO_SPEC_NumberOfBits-112 || cntr <= 0)
            {


            gotsync=preambledetectorreal.Update(bit);
            if(!gotsync_last)
            {
                gotsync_last=gotsync;
                gotsync=0;
            } else gotsync_last=0;

            } else {
                gotsync = 0;
                gotsync_last = 0;

            }

        }
         else
         {

            if(cntr > AERO_SPEC_NumberOfBits-112 || cntr <= 0)
            {

            gotsync=preambledetectorimag.Update(bit);
            if(!gotsync_last)
            {
                gotsync_last=gotsync;
                gotsync=0;
            } else gotsync_last=0;
            } else {
                gotsync = 0;
                gotsync_last = 0;

            }



         }

        if(realimag)
        {
            if(preambledetectorreal.inverted)
            {
                bit=1-bit;

                if(soft_bit > 128)
                {
                     soft_bit = 255-soft_bit;
                } else if (soft_bit < 128)
                {
                    soft_bit = 255-soft_bit;
                }
            }

        }
        else
        {
            if(preambledetectorimag.inverted)
            {
                bit=1-bit;

                if(soft_bit > 128)
                {
                     soft_bit = 255-soft_bit;
                }
                else if (soft_bit < 128)
                {
                     soft_bit = 255-soft_bit;
                }
            }
        }
        if(gotsync)
        {

            // reset the counter, add first bit next time around
            cntr=-1;
            index=-1;

            deleaveredBlock.resize(0);
            depuncturedBlock.clear();
            scrambler.reset();
        }
        else
        {
           // start adding bits to the buffer
           // frame should be complete

            if(cntr<1000000000)cntr++;
            if(cntr<=AERO_SPEC_NumberOfBits-1)
            {
                index++;
                block[index]=soft_bit;
            }
            if(index == (255))
            {
                //deinterleave block
                std::vector<uint8_t> deleaveredblockBA=leaver.deinterleave_ba(block, 4);

                //apend deinterleaved block to total frame
                deleaveredBlock.insert(deleaveredBlock.end(), deleaveredblockBA.begin(), deleaveredblockBA.end());

                // reset for next deleaver block
                index = -1;

            }
            if(cntr==AERO_SPEC_NumberOfBits-1)
            {

                // depuncture the full block
                puncturedCode.depunture_soft_block(deleaveredBlock, depuncturedBlock, 4, true);

                std::vector<int> deconvol=jconvolcodec->Decode_Continuous(depuncturedBlock);

                //resize to drop trailing dummy bits
                deconvol.resize(2714);

                //delay line for frame alignment for non burst modes. This is needed for the scrambler
                dl2.update(deconvol);

                //scrambler
                scrambler.update(deconvol);

                //pack the bits into bytes
                infofield.clear();
                int charptr=0;
                uint8_t ch=0;

                // extract the 24 sub data 12 bit blocks

                for(int y=0; y<24;y++)
                {
                    // offset from start of frame
                    int offset = y * (1+96+12);

                    for(int h=offset+97;h<offset+109;h++)
                    {
                        ch|=deconvol[h]*128;
                        charptr++;charptr%=8;
                        if(charptr==0)
                        {
                            infofield.push_back(ch);//actual data of information field
                            ch=0;
                        }
                        else ch>>=1;
                    }

                    if((int)infofield.size()==12)
                    {

                        bool crcok = false;

                        char *infofieldptr=(char*)infofield.data();
                        uint16_t crc_calc=crc16.calcusingbytes(&infofieldptr[0],12-2);
                        uint16_t crc_rec=(((uint8_t)infofield[12-1])<<8)|((uint8_t)infofield[12-2]);

                        //keep track of the DCD for non burst modes
                        if(crc_calc==crc_rec)
                        {
                            crcok = true;
                            if(datacdcountdown<12)datacdcountdown+=2;
                        }
                        else
                        {
                            if(datacdcountdown>0)datacdcountdown-=5;
                        }
                        if(!datacd&&datacdcountdown>2)
                        {
                            datacd=true;
                            if(dcd_callback) dcd_callback(datacd, dcd_user);
                        }

                        // check for signal unit
                        if(crcok)
                        {
                            /* emit decoded frame data via callback */
                            if(decoded_callback && !infofield.empty())
                            {
                                decoded_callback(infofield.data(), (int)infofield.size(), decoded_user);
                            }
                        }

                        infofield.clear();
                    }


                }// end of sub-data loop

              // Extract C-channel voice: 25 primary fields x 12 bytes (AMBE
              // frames). Each 96-bit field, then skip 13 bits. (From JAERO.)
              if(voice_callback)
              {
                  std::vector<uint8_t> vdata;
                  vdata.reserve(320);
                  int vch=0, vptr=0, bitsin=0;
                  for(int h=1; h<2714; ++h)
                  {
                      vch|=deconvol[h]*128;
                      vptr++; vptr%=8;
                      if(vptr==0){ vdata.push_back((uint8_t)vch); vch=0; }
                      else vch>>=1;
                      bitsin++;
                      if(bitsin==96){ bitsin=0; h+=13; }
                  }
                  for(int i=0;i<25;i++)
                      if((size_t)(i*12+12)<=vdata.size())
                          voice_callback(vdata.data()+i*12, 12, voice_user);
              }

              // reset for next block
              index = -1;
            }// end of frame
        }
    }

    if(!datacd)
    {
        decodedbytes.clear();
    }

    return decodedbytes;
}

void PuncturedCode::depunture_soft_block(std::vector<uint8_t> &sourceblock,std::vector<uint8_t> &targetblock, int pattern,bool reset)
{
    assert(pattern>=2);

    if(reset)depunture_ptr=0;
    for(int i=0;i<(int)sourceblock.size()-1;i++)
    {
        depunture_ptr++;
        targetblock.push_back(sourceblock[i]);
        if(depunture_ptr>=pattern-1)targetblock.push_back(128);
        depunture_ptr%=(pattern-1);
    }

}

PuncturedCode::PuncturedCode()
{
    punture_ptr=0;
    depunture_ptr=0;
}
