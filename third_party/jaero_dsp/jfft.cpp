/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#include "jfft.h"

#include <cstdint>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//using namespace std;

JFFT::JFFT()
{

}

std::map<int, JFFT::TableSet> JFFT::s_pool;
std::mutex JFFT::s_poolMtx;

void JFFT::init(int &fft_size)
{

    //use a bigger FFT size if its not a power of 2
    nfft = 1;
    nfft_2power=0;
    while(nfft < fft_size)
    {
        nfft<<=1;
        nfft_2power++;
    }
    fft_size=nfft;
    //qDebug()<<nfft<<nfft_2power;

    // Check the shared pool for this nfft size
    {
        std::lock_guard<std::mutex> lk(s_poolMtx);
        auto it = s_pool.find(nfft);
        if (it != s_pool.end())
        {
            TWIDDLE_mem     = it->second.tw;
            TWIDDLE_INV_mem = it->second.twInv;
            DIDDLE_A        = it->second.da;
            DIDDLE_B        = it->second.db;
            return;
        }
    }

    // First instance at this nfft: compute tables and register in pool
    auto tw      = std::make_shared<std::vector<cpx_type>>(nfft);
    auto twInv   = std::make_shared<std::vector<cpx_type>>(nfft);
    auto da      = std::make_shared<std::vector<cpx_type>>(nfft);
    auto db      = std::make_shared<std::vector<cpx_type>>(nfft);

    TWIDDLE_mem     = tw;
    TWIDDLE_INV_mem = twInv;
    DIDDLE_A        = da;
    DIDDLE_B        = db;

    //load twiddles (these are just roots of unity, like cutting a cake)
    cpx_type imag=cpx_type(0,1);
    cpx_type *TWIDDLE=TWIDDLE_mem->data();
    cpx_type *TWIDDLE_INV=TWIDDLE_INV_mem->data();
    int w=0;
    for(int N=2;N<=nfft;N<<=1)
    {
        for(int i=0;i<N/2;i++)
        {
            cpx_type twiddle=std::exp(-2.0*imag*M_PI*((double)i)/((double)N));
            cpx_type twiddle_inv=std::exp(2.0*imag*M_PI*((double)i)/((double)N));
            assert(w<nfft);
            TWIDDLE[w]=twiddle;
            TWIDDLE_INV[w]=twiddle_inv;
            w++;
        }
    }

    //load diddle factors
    for(int i=0;i<nfft;i++)
    {
        (*da)[i]=0.5*(1.0-imag*std::exp(-2.0*imag*M_PI*((double)i)/((double)(2*nfft))));
        (*db)[i]=0.5*(1.0+imag*std::exp(-2.0*imag*M_PI*((double)i)/((double)(2*nfft))));
    }

    // Register in shared pool
    {
        std::lock_guard<std::mutex> lk(s_poolMtx);
        s_pool[nfft] = {tw, twInv, da, db};
    }

}

//the size of the sets should be 2 times the size of the fft
void JFFT::fft_real(const double *real,cpx_type *complex,int size)
{
    int NpN=nfft<<1;
    assert(size==NpN);
    F.resize(nfft);

    //split the real data into real and imaginary
    for(int i=0;i<nfft;++i)
    {
        F[i]=cpx_type(real[2*i],real[2*i+1]);
    }

    //perform the complex fft
    fft(F.data(),nfft);

    //do the diddling
    complex[0]=F[0]*(*DIDDLE_A)[0]+(*DIDDLE_B)[0]*std::conj(F[0]);
    complex[nfft]=F[0]*(*DIDDLE_B)[0]+(*DIDDLE_A)[0]*std::conj(F[0]);
    for(int i=1;i<nfft;i++)
    {
        complex[i]=F[i]*(*DIDDLE_A)[i]+(*DIDDLE_B)[i]*std::conj(F[(nfft-i)]);
        complex[NpN-i]=std::conj(complex[i]);
    }
}

//the size of the sets should be 2 times the size of the fft
void JFFT::ifft_real(const cpx_type *complex,double *real,int size)
{
    int NpN=nfft<<1;
    assert(size==NpN);
    F.resize(nfft);

    //do the diddling
    for(int i=0;i<nfft;++i)
    {
        F[i]=complex[i]*std::conj((*DIDDLE_A)[i])+std::conj((*DIDDLE_B)[i])*std::conj(complex[(nfft-i)]);
    }

    //perform the complex inverse fft
    fft(F.data(),nfft,INVERSE);

    //join the real and imaginary data into real
    for(int i=0;i<nfft;++i)
    {
        real[2*i]=F[i].real();
        real[2*i+1]=F[i].imag();
    }
}

void JFFT::fft(cpx_type *x,int size,fft_direction_t fft_direction)
{
    assert(size==nfft);
    cpx_type *TWIDDLE;
    if(fft_direction==FORWARD)TWIDDLE=TWIDDLE_mem->data();
     else TWIDDLE=TWIDDLE_INV_mem->data();

    //for the ifft an alternitive tick is given at http://www.embedded.com/design/configurable-systems/4210789/DSP-Tricks--Computing-inverse-FFTs-using-the-forward-FFT
    //it would mean taking the conjugate of x before the forward fft is done then taking the conjugate after the fft is done
    //this would be a good idea for memory limited devices


    //bit reversal. fast (lets say 2x as fast as the slow one)
    //this does the bit reversal in an iterative way.
    // This is a bit hard to draw in ascii art
    //   1 2 3 4
    //    X   X
    //   2 1 4 3
    //    \ X /
    //     X X
    //    / X \
    //   4 3 2 1
    //
    // or maybe this is a better to describe the process
    //
    //  1 2   3 4   5 6   7 8
    //   X     X     X     X
    //  2 1   4 3   6 5   8 7
    //
    //  21 43   65 87
    //    X       X
    //  43 21   87 65
    //
    //  4321 8765
    //      X
    //  8765 4321
    //
    for (uint32_t i=0;i<((uint32_t)nfft);++i)
    {

        uint32_t y=i;
        y = (((y & 0xaaaaaaaa) >> 1) | ((y & 0x55555555) << 1));
        y = (((y & 0xcccccccc) >> 2) | ((y & 0x33333333) << 2));
        y = (((y & 0xf0f0f0f0) >> 4) | ((y & 0x0f0f0f0f) << 4));
        y = (((y & 0xff00ff00) >> 8) | ((y & 0x00ff00ff) << 8));
        y = ((y >> 16) | (y << 16)) >> (32-nfft_2power);

        if(y>i)
        {
            swap(x[i], x[y]);
            //qDebug()<<y<<i;
        }

    }

    //fft. using pointers harder to understand but is identical to the one in fft_easy_to_understand
    int nfill=0;
    cpx_type y;
    cpx_type *xkp;
    cpx_type *xlp;
    cpx_type *wp;
    for(int n=1;n<nfft;n<<=1)//run through each stage
    {
        int k=0;
        wp=TWIDDLE+n-1;
        xkp=x;
        xlp=x+k+n;
        while(k<nfft)
        {

            //buterfly
            y=(*wp)*(*xlp);
            (*xlp)=(*xkp)-y;
            (*xkp)+=y;

            //find next pointers for the buterfly
            xkp++;xlp++;k++;
            if(k&nfill)wp++;
             else
             {
                k+=n;
                xkp+=n;xlp+=n;
                wp=TWIDDLE+n-1;
             }

        }
        nfill<<=1;
        nfill|=1;
    }

    //scale if we are doing an inverse
    //this only scalling on the ifft matchs what MATLAB does
    if(fft_direction==INVERSE)
    {
        for(int i=0;i<nfft;++i)x[i]*=(1.0/((double)nfft));
    }

}

//this is eaiser to understand
void JFFT::fft_easy_to_understand(cpx_type *x,int size,fft_direction_t fft_direction)
{
    assert(size==nfft);
    cpx_type *TWIDDLE;
    if(fft_direction==FORWARD)TWIDDLE=TWIDDLE_mem->data();
     else TWIDDLE=TWIDDLE_INV_mem->data();

    //for the ifft an alternitive tick is given at http://www.embedded.com/design/configurable-systems/4210789/DSP-Tricks--Computing-inverse-FFTs-using-the-forward-FFT
    //it would mean taking the conjugate of x before the forward fft is done then taking the conjugate after the fft is done
    //this would be a good idea for memory limited devices

    //bit reversal. slow (say 10% of the CPU use for this function is used here)
    for (int i=0;i<nfft;++i)
    {
        int ti=i;
        int ti2=0;
        for(int j=0;j<nfft_2power;++j)
        {
            ti2<<=1;
            ti2|=(ti&1);
            ti>>=1;
        }
        if(ti2>i)
        {
            swap(x[i], x[ti2]);
            //qDebug()<<ti2<<i;
        }
    }

    //fft. most clear
    //look at the image at http://www.alwayslearn.com/DFT%20and%20FFT%20Tutorial/DFTandFFT_FFT_Butterfly_8_Input.html
    //k points to top part of a buterfly and l points to the bottom part of a buterfly. w points to the twiddle that
    //is currently needed. n is for how many buterfly are in a set, stage 1 has 1 stage 2 has 2 stage 3 has 4 stage 4
    //has 8 and so on.
    for(int n=1;n<nfft;n<<=1)//for for each stage
    {
        int k=0;
        int l=k+n;
        int w=n-1;
        while(k<nfft)
        {

            //qDebug()<<k<<l<<w;

            cpx_type y=x[k]-TWIDDLE[w]*x[l];
            x[k]+=TWIDDLE[w]*x[l];
            x[l]=y;

            k++;l++;
            if(!(k%n))
            {
                k+=n;l+=n;
                w=n-1;
            }
             else w++;
        }
    }

    //scale if we are doing an inverse
    //this only scalling on the ifft matchs what MATLAB does
    if(fft_direction==INVERSE)
    {
        for(int i=0;i<nfft;++i)x[i]*=(1.0/((double)nfft));
    }

}

//DFT from definition
//if you were really wanting the best from it you should move Wf to the init function
//but as this is just a rough comparison between a fft and a slow ft implimentation this should do
void JFFT::sft(cpx_type *x,int size,fft_direction_t fft_direction)
{
    cpx_type imag=cpx_type(0,1);
    cpx_type W;
    F.assign(size,0);
    if(fft_direction==FORWARD)W=std::exp(-2.0*imag*M_PI/((double)size));
     else W=std::exp(2.0*imag*M_PI/((double)size));

    //this makes std::pow(W,n*k)==Wf[(n*k)%size] and Wf[(n*k)%size] is faster
    std::vector<cpx_type> Wf;
    Wf.resize(size);
    for(int i=0;i<size;++i)
    {
        Wf[i]=std::pow(W,i);
    }

    for(int n=0;n<size;n++)
    {
        for(int k=0;k<size;k++)
        {
            //F[n]+=x[k]*std::pow(W,n*k);//way way too slow
            F[n]+=x[k]*Wf[(n*k)%size];
        }
    }

    if(fft_direction==INVERSE)
    {
        for(int i=0;i<size;++i)x[i]=F[i]*(1.0/((double)size));
    }
     else
     {
        for(int i=0;i<size;++i)x[i]=F[i];
     }

}

//------------Fast Fir

JFastFir::JFastFir()
{

}

void JFastFir::SetKernel(const JFFT::cpx_type *_kernel,int kernel_size,int approx_fft_size)
{
    //copy kernel over
    kernel.resize(kernel_size);
    memcpy(kernel.data(),_kernel,sizeof(JFFT::cpx_type)*kernel_size);

    //use a bigger FFT size at least 4 x the size of the kernel and make it a power of 2
    kernel_non_zero_size = kernel.size();
    nfft = 1;
    if(approx_fft_size<=0) approx_fft_size = 4*kernel_non_zero_size;//rule of thumb
    while(nfft < approx_fft_size)
    {
        nfft<<=1;
    }

    //pad kernel with zeros till it's nfft in size
    kernel.resize(nfft,0);

    //create a space for the signal to be put
    sigspace.resize(nfft,0);
    sigspace_ptr=0;

    //calulate the signal length per fft
    signal_non_zero_size=nfft+1-kernel_non_zero_size;

    //create a remainder buffer for overlap
    remainder_size=nfft-signal_non_zero_size;
    remainder.resize(remainder_size,0);

    //create real spaces
    sigspace_real.resize(nfft,0);
    remainder_real.resize(remainder_size,0);

    //show the sizes
    //qDebug()<<"kernel_non_zero_size"<<kernel_non_zero_size<<"signal_non_zero_size"<<signal_non_zero_size<<"remainder_size"<<remainder_size<<"nfft"<<nfft;

    //make sure the remainder is not bigger than the signal size.
    assert(remainder_size<=signal_non_zero_size);

    //put the kernel into the freq domain
    fft.fft(kernel);

}

//this is a block processing one and no faster than single processing though
void JFastFir::update_block(JFFT::cpx_type *buffer,int size)
{

    //make a tmp space
    tmp_space.resize(nfft);

    //process data until we have processed the required amount of samples
    int samples_processed=0;
    while(samples_processed<size)
    {

        //if we are back at zero then time for an fft
        if(sigspace_ptr>=signal_non_zero_size)
        {

            if(signal_non_zero_size<=0)return;//check if the fastfir has been initalized. if it hasn't just return what ever we get sent

            //convolution
            psigspace=sigspace.data();
            pkernel=kernel.data();
            fft.fft(sigspace);
            for(int k=0;k<nfft;++k)
            {
                *psigspace*=*pkernel;
                pkernel++;
                psigspace++;
            }
            fft.ifft(sigspace);

            //deal with overlap
            psigspace=sigspace.data();//pointer to sigspace
            premainder=remainder.data();//pointer to remainder
            psigspace_overlap=psigspace+signal_non_zero_size;//pointer to start of the overlap in sigspace
            for(int k=0;k<remainder_size;++k)
            {

                *psigspace+=*premainder;//add last overlap to this sigspace
                *premainder=*psigspace_overlap;//save the remainder from this convolution to remainder
                *psigspace_overlap=0;//the sigspace needs to be padded with zeros once again

                //increse the pointers
                psigspace++;
                premainder++;
                psigspace_overlap++;
            }

            //start from the beginning
            sigspace_ptr=0;

        }

        //calculate the maximum amount of samples we can copy over, that is either the number of samples
        //we still have to process (size-samples_processed) or the number of samples till we fill
        //sigspace (signal_non_zero_size-sigspace_ptr).
        int number_to_copy_over=std::min(signal_non_zero_size-sigspace_ptr,size-samples_processed);
        //qDebug()<<number_to_copy_over;
        memcpy(tmp_space.data(),&buffer[samples_processed],sizeof(JFFT::cpx_type)*number_to_copy_over);//take the unprocessed samples from the buffer and put them in tmp space
        memcpy(&buffer[samples_processed],&sigspace[sigspace_ptr],sizeof(JFFT::cpx_type)*number_to_copy_over);//take the processed samples and put them into the buffer
        memcpy(&sigspace[sigspace_ptr],tmp_space.data(),sizeof(JFFT::cpx_type)*number_to_copy_over);//take the samples from the tmp space and put them into the space for processing
        sigspace_ptr+=number_to_copy_over;
        samples_processed+=number_to_copy_over;

    }


}

//slightly faster but by very little
JFFT::cpx_type JFastFir::update(JFFT::cpx_type in_val)
{
    //if we are back at zero then time for an fft
    if(sigspace_ptr>=signal_non_zero_size)
    {

        if(signal_non_zero_size<=0)return in_val;//check if the fastfir has been initalized. if it hasn't just return what ever we get sent

        //convolution
        psigspace=sigspace.data();
        pkernel=kernel.data();
        fft.fft(sigspace);
        for(int k=0;k<nfft;++k)
        {
            *psigspace*=*pkernel;
            pkernel++;
            psigspace++;
        }
        fft.ifft(sigspace);

        //deal with overlap
        psigspace=sigspace.data();//pointer to sigspace
        premainder=remainder.data();//pointer to remainder
        psigspace_overlap=psigspace+signal_non_zero_size;//pointer to start of the overlap in sigspace
        for(int k=0;k<remainder_size;++k)
        {

            *psigspace+=*premainder;//add last overlap to this sigspace
            *premainder=*psigspace_overlap;//save the remainder from this convolution to remainder
            *psigspace_overlap=0;//the sigspace needs to be padded with zeros once again

            //increse the pointers
            psigspace++;
            premainder++;
            psigspace_overlap++;
        }

        //start from the beginning
        sigspace_ptr=0;

    }

    psigspace=sigspace.data()+sigspace_ptr;
    JFFT::cpx_type out_val=*psigspace;//pop the old val
    *psigspace=in_val;//push in new val

    sigspace_ptr++;

    return out_val;
}

JFFT::cpx_type JFastFir::update_easy_to_understand(JFFT::cpx_type in_val)
{
    //if we are back at zero then time for an fft
    if(sigspace_ptr>=signal_non_zero_size)
    {

        if(signal_non_zero_size<=0)return in_val;//check if the fastfir has been initalized. if it hasn't just return what ever we get sent

        //convolution.
        fft.fft(sigspace);
        for(int k=0;k<nfft;++k)sigspace[k]*=kernel[k];
        fft.ifft(sigspace);

        //this needs remainder_size<=signal_non_zero_size.
        //
        //these 3 can be combined and pointers used.
        //
        //these are used to deal with the the fact that our block of signal
        //data has increased from N to N+M-1 (N is signal size and M is kernel size).
        //we have set it up so N+M-1==nfft and the last M-1 are saved for next time
        //in the remainder buffer. The M-1 samples from the previous time are added to
        //the start of this time. Finally we padd the next signal with zeros to avoid
        //time aliasing. it sonds confusing but it's really not as bad as it sounds.

        //add remainder from last time to the start of this one
        for(int k=0;k<remainder_size;++k)sigspace[k]+=remainder[k];

        //save the remainder of this time for the next one
        for(int k=0;k<remainder_size;++k)remainder[k]=sigspace[k+signal_non_zero_size];

        //clear the end of this for the next fft
        for(int k=0;k<remainder_size;++k)sigspace[k+signal_non_zero_size]=0;

        //start from the beginning
        sigspace_ptr=0;

    }

    JFFT::cpx_type out_val=sigspace[sigspace_ptr];//pop the old val
    sigspace[sigspace_ptr]=in_val;//push in new val

    sigspace_ptr++;

    return out_val;
}

//this could be a bit faster but it's easier to understand this way and the loss of speed is not much
double JFastFir::update(double real_in)
{
    //if we are back at zero then time for an fft
    if(sigspace_ptr>=signal_non_zero_size)
    {

        if(signal_non_zero_size<=0)return real_in;//check if the fastfir has been initalized. if it hasn't just return what ever we get sent

        //convolution.
        fft.fft_real(sigspace_real,sigspace);
        for(int k=0;k<(nfft/2+1);++k)sigspace[k]*=kernel[k];//as it's real only slightly over half of freq is needed
        fft.ifft_real(sigspace,sigspace_real);

        //this needs remainder_size<=signal_non_zero_size.
        //
        //
        //these are used to deal with the the fact that our block of signal
        //data has increased from N to N+M-1 (N is signal size and M is kernel size).
        //we have set it up so N+M-1==nfft and the last M-1 are saved for next time
        //in the remainder buffer. The M-1 samples from the previous time are added to
        //the start of this time. Finally we padd the next signal with zeros to avoid
        //time aliasing. it sonds confusing but it's really not as bad as it sounds.
        for(int k=0;k<remainder_size;++k)
        {
            sigspace_real[k]+=remainder_real[k];//add remainder from last time to the start of this one
            remainder_real[k]=sigspace_real[k+signal_non_zero_size];//save the remainder of this time for the next one
            sigspace_real[k+signal_non_zero_size]=0;//clear the end of this for the next fft
        }

        //start from the beginning
        sigspace_ptr=0;

    }

    double out_real=sigspace_real[sigspace_ptr];//pop the old val
    sigspace_real[sigspace_ptr]=real_in;//push in new val

    sigspace_ptr++;

    return out_real;
}

//----------- Filter design

//---filter design

double JFilterDesign::sinc_normalized(double val)
{
    if (val==0)return 1.0;
    return (std::sin(M_PI*val)/(M_PI*val));
}

std::vector<JFFT::cpx_type> JFilterDesign::LowPassHanning(double FrequencyCutOff, double SampleRate, int Length)
{
    std::vector<JFFT::cpx_type> h;
    if(Length<1)return h;
    if(!(Length%2))Length++;
    int j=1;
    for(int i=(-(Length-1)/2);i<=((Length-1)/2);i++)
    {
        double w=0.5*(1.0-std::cos(2.0*M_PI*((double)j)/((double)(Length))));
        h.push_back(w*(2.0*FrequencyCutOff/SampleRate)*sinc_normalized(2.0*FrequencyCutOff*((double)i)/SampleRate));
        j++;
    }

    return h;

/* in matlab this function is
idx = (-(Length-1)/2:(Length-1)/2);
hideal = (2*FrequencyCutOff/SampleRate)*sinc(2*FrequencyCutOff*idx/SampleRate);
h = hanning(Length)' .* hideal;
*/

}

std::vector<JFFT::cpx_type> JFilterDesign::HighPassHanning(double FrequencyCutOff, double SampleRate, int Length)
{
    std::vector<JFFT::cpx_type> h;
    if(Length<1)return h;
    if(!(Length%2))Length++;

    std::vector<JFFT::cpx_type> h1;
    std::vector<JFFT::cpx_type> h2;
    h2.assign(Length,0);
    h2[(Length-1)/2]=1.0;

    h1=LowPassHanning(FrequencyCutOff,SampleRate,Length);
    if((h1.size()==(size_t)Length)&&(h2.size()==(size_t)Length))
    {
        for(int i=0;i<Length;i++)h.push_back(h2[i]-h1[i]);
    }

    return h;
}

std::vector<JFFT::cpx_type> JFilterDesign::BandPassHanning(double LowFrequencyCutOff,double HighFrequencyCutOff, double SampleRate, int Length)
{
    std::vector<JFFT::cpx_type> h;
    if(Length<1)return h;
    if(!(Length%2))Length++;

    std::vector<JFFT::cpx_type> h1;
    std::vector<JFFT::cpx_type> h2;

    h2=LowPassHanning(HighFrequencyCutOff,SampleRate,Length);
    h1=LowPassHanning(LowFrequencyCutOff,SampleRate,Length);

    if((h1.size()==(size_t)Length)&&(h2.size()==(size_t)Length))
    {
        for(int i=0;i<Length;i++)h.push_back(h2[i]-h1[i]);
    }

    return h;
}

std::vector<JFFT::cpx_type> JFilterDesign::BandStopHanning(double LowFrequencyCutOff,double HighFrequencyCutOff, double SampleRate, int Length)
{
    std::vector<JFFT::cpx_type> h;
    if(Length<1)return h;
    if(!(Length%2))Length++;

    std::vector<JFFT::cpx_type> h1;
    std::vector<JFFT::cpx_type> h2;
    h2.assign(Length,0);
    h2[(Length-1)/2]=1.0;

    h1=BandPassHanning(LowFrequencyCutOff,HighFrequencyCutOff,SampleRate,Length);
    if((h1.size()==(size_t)Length)&&(h2.size()==(size_t)Length))
    {
        for(int i=0;i<Length;i++)h.push_back(h2[i]-h1[i]);
    }

    return h;
}

