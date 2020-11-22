#include "common.h"
#include "BoundedBuffer.h"
#include "Histogram.h"
#include "common.h"
#include "HistogramCollection.h"
#include <thread>
#include <sys/epoll.h>
#include <sys/wait.h>
#include "TCPreqchannel.h"

using namespace std;


void patient_thread_function(int n, int pno, BoundedBuffer* request_buffer){//FIFORequestChannel * chan, HistogramCollection * hc ){
    /* What will the patient threads do? */
    datamsg d(pno, 0.0, 1);
    double resp =0;
    for(int i=0; i<n; i ++ ){
        /*chan-> cwrite(&d,sizeof(datamsg));
        chan->cread(&resp,sizeof(double));
        hc->update(pno,resp);*/
        request_buffer -> push((char*)&d,sizeof(datamsg));
        d.seconds += 0.004;
    }
}


void event_polling_function(int n , int p , int w , int mb , TCPRequestChannel** wchans, BoundedBuffer* request_buffer, HistogramCollection * hc){
    /*
		Functionality of the worker threads	
    */
    char buf[1024];
    double resp =0;
    char recvbuf [mb];
   
    struct epoll_event ev;
    struct epoll_event events[w]; 
    //create epoll list
    int epollfd = epoll_create1(0);
    if(epollfd == -1){
        EXITONERROR("epoll_create1");
    }   

    unordered_map<int,int> fd_to_index;
    vector<vector<char>> state(w);
    //int nsent = 0; nrecv = 0;
    //priming + adding each fd to the list 
    bool quit_recv = false; 

    int nsent =0, nrecv =0 ;
    for(int i=0 ;i<w;i++){
        int sz= request_buffer->pop(buf,1024);
       if(*(MESSAGE_TYPE *)buf == QUIT_MSG) {
            quit_recv = true;
            break;
        }
        wchans[i]->cwrite(buf,sz);
        state[i] = vector<char> ( buf, buf+sz);
        nsent++;
        int rfd= wchans[i]->getfd();

        fcntl(rfd,F_SETFL,O_NONBLOCK);
        ev.events= EPOLLIN | EPOLLET;
        ev.data.fd = rfd;
        fd_to_index [rfd] = i;
        if(epoll_ctl(epollfd, EPOLL_CTL_ADD,rfd, &ev)== -1){
            EXITONERROR("epoll_ctl: listen_sock");
        }
    }

    while(true){
        //if(quit_recv && nsent == nrecv){
        if(quit_recv && nrecv == nsent){
            break;
        }
        int nfds= epoll_wait(epollfd,events,w,-1);
        if(nfds==-1){
            EXITONERROR("epoll_wait");
        }

        for(int i=0 ; i<nfds;i++){
            int rfd = events [i].data.fd;
            int index = fd_to_index[rfd];
            int resp_sz = wchans[index] ->cread(recvbuf,mb);
            nrecv ++;
            //process(recvbuf)
            vector<char> req = state [index];
       
            char* request = req.data();

            //processing the response
            MESSAGE_TYPE* m = (MESSAGE_TYPE *)request;
            if(*m==DATA_MSG){

                hc->update(((datamsg*)request)->person, *(double*)recvbuf);//resp);
            }else if(*m == FILE_MSG){
                filemsg* fm = (filemsg*) request;
                string fname = (char*)(fm + 1);

                string recvfname = "recv/" + fname;
                FILE* fp = fopen(recvfname.c_str(),"r+");
                fseek(fp,fm->offset, SEEK_SET);
                fwrite(recvbuf,1,fm->length,fp);
                fclose(fp);
            }

            //reuse
            if( !quit_recv ){
                int req_sz = request_buffer->pop (buf,sizeof(buf));
                if(*(MESSAGE_TYPE *) buf == QUIT_MSG){
                    quit_recv = true;
                    //continue;
                }
                else{
                    wchans[index] ->cwrite(buf,req_sz);
                    state[index] =vector<char> (buf,buf+req_sz);
                    nsent++;
                }
            }
        }           
    }

}

void file_thread_function(string fname, BoundedBuffer* request_buffer,TCPRequestChannel * chan, int mb){
    //1.create the file
    string recvfname = "recv/" + fname;
    
    //2. generate all the file messages
    char buf[1024];
    filemsg f(0,0);
    memcpy(buf,&f,sizeof(f));
    strcpy(buf + sizeof(f),fname.c_str());
    chan->cwrite(buf, sizeof(f) + fname.size() + 1);
    __int64_t filelength;
    chan->cread(&filelength,sizeof(filelength));

    FILE*  fp= fopen(recvfname.c_str(),"w");
    fseek(fp,filelength,SEEK_SET);
    fclose(fp);

    filemsg* fm = (filemsg*) buf;
    __int64_t remlen = filelength;

    while(remlen > 0){
        fm->length = min(remlen,(__int64_t) mb);
        request_buffer->push(buf,sizeof(filemsg)+fname.size()+1);
        fm->offset += fm->length;
        remlen -=fm->length;
    }

}

int main(int argc, char *argv[])
{
    int n = 15;   // default number of requests per "patient"
    int p = 10;      // number of patients [1,15]
    int w = 10;    // default number of worker threads
    int b = 100; 	// default capacity of the request buffer, you should change this default
	int m = MAX_MESSAGE; 	// default capacity of the message buffer
    srand(time_t(NULL));
    string filename = "";
    bool p_request = false;
    bool f_request = false;
    string m_s = "";
    string host, port;

    // Grab command line arguments
    int opt;
    while((opt = getopt(argc, argv, "m:n:p:b:w:f:h:r:")) != -1) {
        switch(opt) {
            case 'm':
                m_s = optarg;
                m = atoi(optarg);
                break;
            case 'n':
                n = atoi(optarg);
                break;
            case 'p':
                p = atoi(optarg);
                p_request = true;
                break;
            case 'b':
                b = atoi(optarg);
                break;
            case 'w':
                w = atoi(optarg);
                break;
            case 'f':
                filename = optarg;
                f_request = true;
                break;
            case 'h':
                host = optarg;
                break;
            case 'r':
                port = optarg;
                break;
        }
    }

    if(m_s == "") {
        m_s = "256";
    }

    BoundedBuffer request_buffer(b);
	HistogramCollection hc;

	// Initializing histograms and adding them to hc
    for(int i = 0; i < p; i++) {
        Histogram* h = new Histogram(10, -2.0, 2.0);
        hc.add(h);
    }

    struct timeval start, end;
    gettimeofday (&start, 0);

    TCPRequestChannel* workerChan[w];

    /* Start all threads here */
    if(p_request) {
        for(int i = 0; i < w; i++) {
            workerChan[i] = new TCPRequestChannel(host, port);
        }

        thread patient[p];
        for(int i = 0; i < p; i++) {
            patient[i] = thread(patient_thread_function, n, i + 1, &request_buffer);
        }

        thread evp(event_polling_function, n, p, w, m, (TCPRequestChannel**)workerChan, &request_buffer, &hc);

        for(int i = 0; i < p; i++) {
            patient[i].join();
        }
        cout << "Patient threads completed" << endl;

        MESSAGE_TYPE q = QUIT_MSG;
        request_buffer.push((char*)&q, sizeof(q));

        evp.join();
        cout << "Worker threads completed" << endl;
    }
    if(f_request) {
        for(int i = 0; i < w; i++) {
            workerChan[i] = new TCPRequestChannel(host, port);
        }

        thread filethread(file_thread_function, filename, &request_buffer, workerChan[0], m);

        thread evp(event_polling_function, n, p, w, m, (TCPRequestChannel**)workerChan, &request_buffer, &hc);

        filethread.join();

        MESSAGE_TYPE q = QUIT_MSG;
        request_buffer.push((char*)&q, sizeof(q));

        evp.join();
        cout << "Worker threads completed" << endl;
    }

    gettimeofday (&end, 0);
    // print the results
	hc.print();

    int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int) 1e6;
    int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int) 1e6);
    cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;

    MESSAGE_TYPE q = QUIT_MSG;

    for(int i = 0; i < w; i++) {
        workerChan[i]->cwrite((char *)&q, sizeof(MESSAGE_TYPE));
        delete workerChan[i];
    }
    cout << "All worker channels deleted" << endl;
}