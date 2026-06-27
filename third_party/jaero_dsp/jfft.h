/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#ifndef JFFT_H
#define JFFT_H

#include <complex>
#include <cmath>
#include <cstdint>
#include <cassert>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#ifdef QT_CORE_LIB
#include <QDebug>
#include <QVector>
#endif

//Radix 2. Floating complex 1 dimentional in place FFT/IFFT
//included is also a SFT (Slow Fourier Transform) on my desktop a 16384 point transform took 1.2ms for my FFT and
//3.4s for my SFT. Thats about 3000 times faster
//We also have real 1 dimentional non in place FFT/IFFT that are about twice as fast as the complex ones
class JFFT
{
public:
    typedef std::complex<double> cpx_type;//this is the complex number definition. people really should use complex numbers more they are much better than real numbers
    typedef enum fft_direction_t{FORWARD,INVERSE}fft_direction_t;
    JFFT();
    void init(int &fft_size);

    //real ffts. NB the underlying complex FFT needs to be half of the size of size
    void fft_real(const double *real,cpx_type *complex,int size);
    void ifft_real(const cpx_type *complex,double *real,int size);

    void fft(cpx_type *x,int size,fft_direction_t fft_direction=FORWARD);//this is the main fft/ifft function and is the fastest
    void fft_easy_to_understand(cpx_type *x,int size,fft_direction_t fft_direction=FORWARD);//this is the same but is slower and eisier to undrstand
    void sft(cpx_type *x,int size,fft_direction_t fft_direction=FORWARD);//a slow dft just for comparison
    int get_nfft(){return nfft;}

    //convenience functions and others if you want to add them. these are a few of the ones I use

    //convenience functions std::vector form
    void fft_real(std::vector<double> &real,std::vector<cpx_type> &complex)
    {
        int tnfft=real.size()>>1;
        if(tnfft!=nfft)
        {
            init(tnfft);
            real.resize(2*tnfft,0);
            complex.resize(2*tnfft,0);
        }
        if(complex.size()!=real.size())complex.resize(real.size());
        fft_real(real.data(),complex.data(),real.size());
    }
    void ifft_real(std::vector<cpx_type> &complex,std::vector<double> &real)
    {
        int tnfft=complex.size()>>1;
        if(tnfft!=nfft)
        {
            init(tnfft);
            real.resize(2*tnfft,0);
            complex.resize(2*tnfft,0);
        }
        if(complex.size()!=real.size())real.resize(complex.size());
        ifft_real(complex.data(),real.data(),real.size());
    }
    void fft(std::vector<cpx_type> &x)
    {
        if(((int)x.size())!=nfft)
        {
            int tnfft=x.size();
            init(tnfft);
            x.resize(tnfft,0);
        }
        fft(x.data(),x.size());
    }
    void ifft(std::vector<cpx_type> &x)
    {
        if(((int)x.size())!=nfft)
        {
            int tnfft=x.size();
            init(tnfft);
            x.resize(tnfft,0);
        }
        fft(x.data(),x.size(),INVERSE);
    }

#ifdef QT_CORE_LIB    
    //convenience functions QVector form
    void fft_real(QVector<double> &real,QVector<cpx_type> &complex)
    {
        int tnfft=real.size()>>1;
        if(tnfft!=nfft)
        {
            init(tnfft);
            real.resize(2*tnfft);
            complex.resize(2*tnfft);
        }
        if(complex.size()!=real.size())complex.resize(real.size());
        fft_real(real.data(),complex.data(),real.size());
    }
    void ifft_real(QVector<cpx_type> &complex,QVector<double> &real)
    {
        int tnfft=complex.size()>>1;
        if(tnfft!=nfft)
        {
            init(tnfft);
            real.resize(2*tnfft);
            complex.resize(2*tnfft);
        }
        if(complex.size()!=real.size())real.resize(complex.size());
        ifft_real(complex.data(),real.data(),real.size());
    }
    void fft_real(const QVector<double> &real,QVector<cpx_type> &complex)
    {
        int tnfft=real.size()>>1;
        if(tnfft!=nfft)init(tnfft);
        assert(tnfft==nfft);
        if(complex.size()!=real.size())complex.resize(real.size());
        fft_real(real.data(),complex.data(),real.size());
    }
    void ifft_real(const QVector<cpx_type> &complex,QVector<double> &real)
    {
        int tnfft=complex.size()>>1;
        if(tnfft!=nfft)init(tnfft);
        assert(tnfft==nfft);
        if(complex.size()!=real.size())real.resize(complex.size());
        ifft_real(complex.data(),real.data(),real.size());
    }
    void fft(QVector<cpx_type> &x)
    {
        if(x.size()!=nfft)
        {
            int tnfft=x.size();
            init(tnfft);
            x.resize(tnfft);
        }
        fft(x.data(),x.size());
    }
    void ifft(QVector<cpx_type> &x)
    {
        if(x.size()!=nfft)
        {
            int tnfft=x.size();
            init(tnfft);
            x.resize(tnfft);
        }
        fft(x.data(),x.size(),INVERSE);
    }
#endif

private:

    //size in both power of 2 and number
    int nfft_2power=0;
    int nfft=0;

    //memory — shared across instances with the same nfft via a static pool
    using Vcpx = std::shared_ptr<std::vector<cpx_type>>;
    Vcpx DIDDLE_A;
    Vcpx DIDDLE_B;
    Vcpx TWIDDLE_mem;
    Vcpx TWIDDLE_INV_mem;
    std::vector<cpx_type> F;//used if the slow DFT is done or real FFT/iFFT

    // Static pool: one set of tables per nfft size
    struct TableSet { Vcpx tw, twInv, da, db; };
    static std::map<int, TableSet> s_pool;
    static std::mutex s_poolMtx;

    void inline swap(cpx_type &a,cpx_type &b){cpx_type c_tmp=b;b=a;a=c_tmp;}


};

//----------------


//an example of 1D FastFir (1D Fast convolution)
//I have not yet taken advantage of the real ffts above.
class JFastFir
{
public:
    JFastFir();
    void SetKernel(const JFFT::cpx_type *kernel,int size,int approx_fft_size=-1);
    void update_block(JFFT::cpx_type *buffer,int size);//process a block at a time this may not be any faster than the convenience function
    JFFT::cpx_type update(JFFT::cpx_type in_val);//process one sample at a time
    JFFT::cpx_type update_easy_to_understand(JFFT::cpx_type in_val);//process one sample at a time. easy to understand
    double update(double real_in);//process one real sample fast one at a time

    //convenience functions
    void update(JFFT::cpx_type *buffer,int size)//process a block at a time version 1
    {
        for(int i=0;i<size;++i)
        {
            buffer[i]=update(buffer[i]);
        }
    }
    void update(std::vector<JFFT::cpx_type> &buffer)//process a block at a time
    {
        update(buffer.data(),buffer.size());
    }
#ifdef QT_CORE_LIB
    void update(QVector<JFFT::cpx_type> &buffer)//process a block at a time
    {
        update(buffer.data(),buffer.size());
    }
    void SetKernel(const QVector<JFFT::cpx_type> &_kernel,int approx_fft_size=-1)//for a complex kernel as a vector
    {
        SetKernel(_kernel.data(),_kernel.size(),approx_fft_size);
    }
    void SetKernel(const QVector<double> &_kernel,int approx_fft_size=-1)//for a real kernel
    {
        QVector<JFFT::cpx_type> tmp_kernel;
        tmp_kernel.resize(_kernel.size());
        for(int i=0;i<((int)_kernel.size());++i)tmp_kernel[i]=_kernel[i];
        SetKernel(tmp_kernel,approx_fft_size);
    }
#endif
    void SetKernel(const std::vector<JFFT::cpx_type> &_kernel,int approx_fft_size=-1)//for a complex kernel as a vector
    {
        SetKernel(_kernel.data(),_kernel.size(),approx_fft_size);
    }
    void SetKernel(const std::vector<double> &_kernel,int approx_fft_size=-1)//for a real kernel
    {
        std::vector<JFFT::cpx_type> tmp_kernel;
        tmp_kernel.resize(_kernel.size());
        for(int i=0;i<((int)_kernel.size());++i)tmp_kernel[i]=_kernel[i];
        SetKernel(tmp_kernel,approx_fft_size);
    }
    double update_real_slow(double in_val)//process one sample at a time for a real signal
    {
        JFFT::cpx_type tmp_in_val=in_val;
        return update(tmp_in_val).real();
    }

private:
    JFFT fft;
    std::vector<JFFT::cpx_type> kernel;
    std::vector<JFFT::cpx_type> sigspace;//in out buffer
    std::vector<JFFT::cpx_type> remainder;//used for overlap

    JFFT::cpx_type *pkernel;
    JFFT::cpx_type *psigspace;
    JFFT::cpx_type *premainder;
    JFFT::cpx_type *psigspace_overlap;

    int kernel_non_zero_size=0;
    int remainder_size=0;

    //this is for in and out buffer
    int signal_non_zero_size=0;
    int sigspace_ptr=0;

    int nfft=0;//fft size

    //space if using real filtering
    std::vector<double> sigspace_real;//in out real buffer
    std::vector<double> remainder_real;//used for real overlap

    //for block prosessing using version 2
    std::vector<JFFT::cpx_type> tmp_space;


};

//------------------------


//filter design
//all designs are using the window method and derived from the low pass filter

class JFilterDesign
{
public:
    JFilterDesign(){}
    static std::vector<JFFT::cpx_type> LowPassHanning(double FrequencyCutOff, double SampleRate, int Length);
    static std::vector<JFFT::cpx_type> HighPassHanning(double FrequencyCutOff, double SampleRate, int Length);
    static std::vector<JFFT::cpx_type> BandPassHanning(double LowFrequencyCutOff,double HighFrequencyCutOff, double SampleRate, int Length);
    static std::vector<JFFT::cpx_type> BandStopHanning(double LowFrequencyCutOff,double HighFrequencyCutOff, double SampleRate, int Length);
private:
    static double sinc_normalized(double val);
};

#endif // JFFT_H
